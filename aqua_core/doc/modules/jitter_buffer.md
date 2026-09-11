# 模块：JitterBuffer

## 文件

- `aqua_core/include/aqua/audio/buffer/jitter_buffer.h`
- `aqua_core/src/audio/buffer/jitter_buffer.cpp`

算法细节（水位分层、Fill/Drop episode、reanchor 判定时机）见 `../buffer_design.md`。本文描述模块接口、配置与接线。

## 职责

JitterBuffer 是 Client playback path 上**唯一**的应用层缓冲，同时承担：

1. UDP 乱序重排（按 `sequence % N` 入槽，天然重排）；
2. 丢帧检测与静音补齐；
3. 启动 pre-roll；
4. 低水位时暂缓播放建立积压（Fill）；
5. 高水位时跳过未来 slot 降低积压（Drop）；
6. 时间线明显跳变时安全 reanchor；
7. **Phase 2**：缺帧时按"重复上一个有效包 + 线性淡出"掩盖（详见下文 §concealment）。

它不是"按毫秒睡眠"的缓冲：容量与调整动作的基本单位都是 **slot**。

## 自适应 target（Phase 1，默认开）

固定 target（0.60）之外，`TargetController` 按到达抖动动态调 target：

```text
target = clamp((base_delay + k×J) / packet_ms, min=max(3, geometric_floor+1) + 欠载惩罚, max=capacity)
```

涨立即跟进（**无死区**，避免卡在 desired−1），跌按 1 格/秒限速。水位带
（warning/normal）以 target 为基准按构造比例跟随，带区间不脱钩；Fill/Drop/
reanchor 状态机本身不变，只是"偏低/偏高"的分界动了。起步水位
`max(3, 初值)` slots，初值 4 slots。

### 欠载反馈（细则 §3：underrun history 是 controller 的输入）

预测项 k×J 用的是**均值**，覆盖不了随机抖动的尾部，更覆盖不了丢包——这两类
情况在真实网络里都会漏成欠载。反馈项补这个洞：JB 的 `underrun_events`
单调递增计数器由 push strand 读快照做增量，每次欠载把 target 的**下限**顶
高 1 槽（累计上限 6 槽），欠载一停即按 0.5 槽/秒连续回落。

抬的是下限而不是往 margin 上叠加：k×J 已经很高时不重复放大，只有 k×J 失算
时下限才真正起作用。干净链路上 penalty 恒为 0，不增加任何延迟——它是安全网，
不是主力。反馈项与跌侧限速叠加，回落一定慢于抬升。

### 为什么 k=5 而不是教科书的 2~3

J（RFC 3550 A.8）是到达间隔偏差的**均值**，而 target 必须覆盖**峰值**。Aqua
的 server 以 capture 周期成串发包：`AudioNetworkDispatcher` 的 worker 把
capture callback 产出的包**背靠背全速发完**（480 帧/10 ms 抓一次、180 帧/包 →
每 10 ms 一串 2~3 个包，串内到达间隔≈0）。这种确定性 burst 下实测
`jit_ms≈4.6 ms`，而 transit 峰峰值 8.75 ms ≈ 均值的 2 倍；再叠加播放 callback
周期（WASAPI 实测 512 帧 = 10.667 ms）与发包周期（10 ms）的拍频（160 ms 一轮
扫过全部相位），k=2 给出的 3 slots 在最坏相位上必然周期性排空。

**`geometric_floor+1` 是几何地板**：一次 playback callback 消耗
`ceil(callback_frames / F)` 个包（512/180 → 3），target 不高于它就意味着"每个
callback 都必然把 JB 抽空"——与抖动无关的结构性错误，所以 target 至少
grant+1（一个 callback 的口粮 + 一包垫到达相位）。F=180@48k 时地板 = 4。

> 离线仿真（同几何，扫 16 个相位取最坏，`tools/sim_jb_target.py`）：
> k=5 → 理想有线 T=7(26 ms) 欠载 0.000%；+1 ms 抖动 T=8 欠载 0.13%；
> +3 ms T=9 欠载 0.05%。k=4 在理想有线就掉到 T=5 / 12.7% 欠载。
> 若 server 把 burst 摊平成匀速发包，同一套参数自动落到地板 4 slots(15 ms)。

- 开关：CLI `--jb-fixed-target` / C API `jb_fixed_target != 0` 切回
  既有固定水位；`ClientRuntimeConfig::jb_adaptive_target`（默认 true）。

### 可调参数

完整的参数语义、原理与调参决策流程见设计文档
`../jitter_buffer_adaptive_design.md` 附录 A。CLI 面向高级玩家，全部暴露：

- **主力**：`--jb-jitter-gain`（k，默认 5）、`--jb-underrun-penalty`（默认 1.0）。
- **高级**：`--jb-capacity`、`--jb-min-target`、`--jb-initial-target`、
  `--jb-fall-rate`、`--jb-rise-dwell`（涨后锁跌，默认 3000ms）、
  `--jb-underrun-penalty-max`、`--jb-underrun-decay`、`--jb-conceal-max`、
  `--jb-stall-threshold`。
- **开关**：`--jb-fixed-target`、`--jb-no-conceal`。
- 观测：`JitterEstimator`（RFC 3550 J + 相对 transit + 底噪最小值），只进诊断。
  **stall 与抖动分离**：到达间隔 > 5 个包周期判为断流，不进 J（否则一次
  170ms 的 Wi-Fi stall 会把 target 从 7 顶到 21 挂 14s），只计 `stall_events`
  并打日志，交由欠载反馈/reanchor 负责。
- 诊断：快照 `target_slots`/`target_ms` 与 `lead_slots`、`estimator_jitter_ms`
  同快照可读；target 每次变化在 push strand 上打一条 debug 日志
  （`adaptive target: 4 -> 7 slots ... jit_ms= base_ms= underrun_penalty=
  bands[wl= nl= nh= wh=]`），水位带一并打出，可事后回溯变化原因。

## PCM concealment（Phase 2，产品默认开 / 组件默认关）

缺帧时把"上一真实 slot 的 PCM 重复一次"作为占位输出，连续掩盖超上限后退回
静音；**不**修改 sequence/timestamp，**不**参与 target/estimator 的统计语义。

- 配置：`JitterBufferConfig::concealment { enabled, max_slots=3 }`。
  组件默认 `enabled=false`（v1 静音行为不变），`ClientRuntimeConfig::jb_pcm_concealment`
  （默认 true）通过 `setup_playback` 透传。
- 增益：第 i 个被掩盖的包（0-indexed）增益 = `(max_slots - i) / max_slots`，
  线性淡出（Q15 定点，循环外预计算，循环内免分支快路径）。
- 封顶：第 `max_slots + 1` 个起输出静音（细则 §9 强制上限）。
- 末状态：出现真实 PCM 立即复位 `have_last_pcm_/conceal_active_/conceal_run_`，
  `last_pcm_` 保留供下一缺帧 run 复用。
- 迟到包：仍 drop（生产侧 0 改动），但 producer 单独记录 `late_useful_packets`
  —— 落后播放头 ≤ `max_slots` 即"本可用"（细则 §14 留口子，本阶段不实际插入）。

RT 契约：所有掩盖状态（`last_pcm_/conceal_active_/conceal_run_`）是 consumer
线程私有；`last_pcm_` 在构造期 `resize(slot_bytes_)` 预分配，pull 路径不分配；
不持 ring slot 指针（producer 可能已回收并覆写），改存消费侧副本。

## 几何

```text
N = capacity_slots           默认 30（CLI --jb-capacity，范围 4..4096）
F = frame_count              来自 ConnectResponse
B = format.frame_bytes()
S = F × B                    一个 slot 的 PCM 字节数
C = N × S                    storage 字节容量
```

环形存储有 N 个 slot，每个预分配 S 字节；构造完成后 `push()` / `pull()` 不再分配。

## 接口

```cpp
static std::expected<std::unique_ptr<JitterBuffer>, AudioError>
create(const JitterBufferConfig&);

bool push(const AudioFrame& frame) noexcept;              // producer：网络线程
JitterBufferPullResult pull(std::span<std::byte>) noexcept; // consumer：回放 RT 线程
```

`push()` 返回 false 表示丢弃（未对齐、迟到、槽冲突/重复、或跨度超过 reanchor 允许的荒谬值）。远超前帧
（`s >= play_seq + N`）不再直接丢弃，而是记录 reanchor 请求，由 consumer 在 `pull()` 中择机应用。

`pull()` 的 `output` 必须按 `frame_bytes` 对齐，否则直接返回 0。正常路径始终填满 output：

```text
真实 PCM + 缺帧静音 + 低水位强制 Hold 静音
+（concealment 开启时）缺帧 slot 重复上一个有效 slot 的 PCM 并按线性淡出
```

静音字节按会话编码（`AudioFormat::silence_byte()`：U8=0x80，其余=0x00）。pre-roll 等待与 Hold 路径
同样计入 `pull_frames/pull_silence_frames`；每次 `pull` 按本批静音帧数结算 `record_silence_run`
（0 即出现真实数据，归零连续 run），`consecutive/max_silence_run` 可用于区分抖动与 blackout。

concealment 路径的输出**不算静音**（计入 `underrun_frames` 但**不**计入 `pull_silence_frames`），
便于在诊断中区分"在掩盖的欠载"与"真正静音的欠载"。

这样后端不会因为 callback 未填满而重复播放上一次缓冲的残留数据。

`JitterBufferPullResult{frames_filled, silence_frames, skipped_slots}` 分别对应：实际填充帧数、其中静音帧数、本次跳过的
槽数（Drop）。

### 配置

| 字段            | 默认 | 含义                                        |
|-----------------|------|-----------------------------------------------|
| `capacity_slots`| 30   | 环形槽数 N（下限 4）                          |
| `format`        | —    | 权威 PCM 格式（必填）                         |
| `frame_count`   | —    | 每帧 sample frame 数 F（必填，来自 server）   |
| `concealment`   | off  | Phase 2 PCM concealment（`enabled` + `max_slots=3`） |
| `target`        | 0.60 | 恢复目标 / 稳态中心                           |
| `normal_low`    | 0.35 | normal 下界                                   |
| `normal_high`   | 0.80 | normal 上界                                   |
| `warning_low`   | 0.20 | warning / deadline 下分界                     |
| `warning_high`  | 0.90 | warning / deadline 上分界                     |
| `startup_level` | 0.50 | 启动 pre-roll 锚定水位                        |
| `step` / `step_fn` | — | warning 区步长参数与步长函数（函数指针，无状态）|

## SPSC 角色

```text
UDP / network thread ── push() ──► producer
AAudio / WASAPI playback RT ── pull() ──► consumer
```

`push()` 与 `pull()` 都不使用互斥锁；所有消费侧 episode 状态由 consumer 线程私有持有。

## 在 ClientRuntime 中的接线

- 创建：`start()` 阶段用 gRPC ConnectResponse 的 format / F 构造，先于 playback 启动；
- 生产：UDP 接收回调（transport strand）构造 `AudioFrame` 后 `push()`；
- 消费：playback 回调调 `ClientRuntime::pull_playback()` → `JitterBuffer::pull()`；
- 销毁：`stop_locked()` 中 `jb_.reset()`。

`push()` 的返回值在当前路径中被忽略——丢弃原因全部通过计数器暴露（见 `../buffer_design.md` §12）。

## 实时约束

`pull()` 禁止：mutex、堆分配、系统调用、阻塞等待、同步日志。`AQUA_JITTER_BUFFER_RT_DEBUG_LOG` 是开发期开关，开启会破坏 RT
契约，不可用于生产构建。

## 测试

`tests/audio/` 下：`jitter_buffer_test.cpp`（基础与边界）、`jitter_buffer_boundary_test.cpp`（水位边界）、
`jitter_buffer_recovery_regression_test.cpp`（排空后重新入帧不静默饿死等回归）、
`jitter_estimator_test.cpp`（JitterEstimator 单测）、`target_controller_test.cpp`（TargetController 单测）、
`jitter_buffer_concealment_test.cpp`（Phase 2 concealment + 欠载/迟到计数）。
