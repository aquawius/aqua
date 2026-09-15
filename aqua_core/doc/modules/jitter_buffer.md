# 模块：JitterBuffer

## 文件

- `aqua_core/include/aqua/audio/buffer/jitter_buffer.h`
- `aqua_core/src/audio/buffer/jitter_buffer.cpp`

算法细节（水位分层、Fill/Drop episode、reanchor 判定时机）见 `../buffer_design.md`。本文描述模块接口、配置与接线。

## 职责

JitterBuffer 是 Client playback path 上 **唯一**的应用层缓冲，同时承担：

1. UDP 乱序重排（按 `sequence % N` 入槽，天然重排）；
2. 丢帧检测与静音补齐；
3. 启动 pre-roll；
4. 低水位时暂缓播放建立积压（Fill）；
5. 高水位时跳过未来 slot 降低积压（Drop）；
6. 时间线明显跳变时安全 reanchor；
7. **Phase 2**：缺帧时按"重复上一个有效包 + 线性淡出"掩盖（详见下文 §concealment）。

它不是"按毫秒睡眠"的缓冲：容量与调整动作的基本单位都是 **slot**。

## 自适应 target（默认开）

固定 target（0.60N）之外，`TargetController` 按到达抖动动态调 target。 **控制律公式、参数推导、ADR 与实验矩阵见
`../jitter_buffer_control_design.md`；数值见 `../configuration_reference.md`。** 本节只写接线与边界。

```text
target = ceil( clamp( margin_slots, effective_min, 2/3 × capacity ) )    # 完整公式见 design §5.1
```

- **数据流**：在 push strand 上，`UdpClient` 的 arrival observer 依次调用
  `JitterEstimator::observe` → `TargetController::update` → `JitterBuffer::set_target_slots`。
  `set_target_slots` 只写一个原子；四个水位带由构造期预计算的带表按 target 现算，不存在"target 已更新、带还是旧值"
  的撕裂窗口。执行层状态机（Fill/Drop/reanchor/concealment）不变，只是"偏低/偏高"的分界动了。
- **几何地板**：`max(--jb-min-target, ceil(callback_frames / F) + 1)`。callback 帧数取 `pull_playback` 观测的 **实际**
  请求量（RT 线程原子缓存 + 控制线程 500ms 轮询比对校正）；`AudioStreamInfo::frames_per_burst` 在 WASAPI legacy 下恒
  0，不可作此口径。
- **开关**：CLI `--jb-fixed-target` / C API `jb_fixed_target != 0` 切回固定水位，对应
  `ClientRuntimeConfig::jb_adaptive_target`（默认 true）。关掉时 `TargetController` **根本不创建**。
- **CLI 旋钮**：`--jb-capacity`、`--jb-jitter-gain`、`--jb-min-target`，以及 Wave A 新增的
  `--jb-stall-peak-cap` / `--jb-stall-decay` / `--jb-stall-threshold` / `--jb-underrun-penalty`。其余为
  `buffer_config.h` 常量（候选清单与结构性约束见 `../configuration_reference.md` §5.2/§5.3）。
- **诊断**：快照 `target_slots` / `target_ms` 与 `lead_slots`、`estimator_jitter_ms` 同快照可读；
  `ClientRuntime adaptive target:` 行（每次变化）给出 margin 胜出方（`src`）与夹持状态（`floor_bind`/`cap_bind`）。 决策层细粒度日志（
  `TargetController change/steady`、`JitterEstimator stall`、`JitterBuffer reanchor probe` 等） 需要
  `AQUA_JB_CONTROL_THREAD_DEBUG_LOG`，点位全表与排查指引见 `observability.md`。

## PCM concealment（Phase 2，产品默认开 / 组件默认关）

缺帧时把"上一真实 slot 的 PCM 重复一次"作为占位输出，连续掩盖超上限后退回 静音； **不**修改 sequence/timestamp， **不**参与
target/estimator 的统计语义。

- 配置：`JitterBufferConfig::concealment { enabled, max_slots=3 }`。 组件默认 `enabled=false`（v1 静音行为不变），
  `ClientRuntimeConfig::jb_pcm_concealment`
  （默认 true）通过 `setup_playback` 透传。
- 增益：第 i 个被掩盖的包（0-indexed）增益 = `(max_slots - i) / max_slots`， 线性淡出（Q15 定点，循环外预计算，循环内免分支快路径）。
- 封顶：第 `max_slots + 1` 个起输出静音（细则 §9 强制上限）。
- 末状态：出现真实 PCM 立即复位 `have_last_pcm_/conceal_active_/conceal_run_`，
  `last_pcm_` 保留供下一缺帧 run 复用。
- 迟到包：仍 drop（生产侧 0 改动），但 producer 单独记录 `late_useful_packets`
  —— 落后播放头 ≤ `max_slots` 即"本可用"（细则 §14 留口子，本阶段不实际插入）。

RT 契约：所有掩盖状态（`last_pcm_/conceal_active_/conceal_run_`）是 consumer 线程私有；`last_pcm_` 在构造期
`resize(slot_bytes_)` 预分配，pull 路径不分配； 不持 ring slot 指针（producer 可能已回收并覆写），改存消费侧副本。

## 几何

```text
N = capacity_slots           默认 30（CLI --jb-capacity，范围 4..512）
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

`push()` 返回 false 表示丢弃（未对齐、迟到、槽冲突/重复、或跨度超过 reanchor 允许的荒谬值）。远超前帧 （`s >= play_seq + N`
）不再直接丢弃，而是记录 reanchor 请求，由 consumer 在 `pull()` 中择机应用。

`pull()` 的 `output` 必须按 `frame_bytes` 对齐，否则直接返回 0。正常路径始终填满 output：

```text
真实 PCM + 缺帧静音 + 低水位强制 Hold 静音
+（concealment 开启时）缺帧 slot 重复上一个有效 slot 的 PCM 并按线性淡出
```

静音字节按会话编码（`AudioFormat::silence_byte()`：U8=0x80，其余=0x00）。pre-roll 等待与 Hold 路径 同样计入
`pull_frames/pull_silence_frames`；每次 `pull` 按本批静音帧数结算 `record_silence_run`
（0 即出现真实数据，归零连续 run），`consecutive/max_silence_run` 可用于区分抖动与 blackout。

concealment 路径的输出 **不算静音**（计入 `underrun_frames` 但 **不**计入 `pull_silence_frames`），
便于在诊断中区分"在掩盖的欠载"与"真正静音的欠载"。

这样后端不会因为 callback 未填满而重复播放上一次缓冲的残留数据。

`JitterBufferPullResult{frames_filled, silence_frames, skipped_slots}` 分别对应：实际填充帧数、其中静音帧数、本次跳过的
槽数（Drop）。

### 配置

| 字段               | 默认 | 含义                                                 |
|--------------------|------|------------------------------------------------------|
| `capacity_slots`   | 30   | 环形槽数 N（下限 4）                                 |
| `format`           | —    | 权威 PCM 格式（必填）                                |
| `frame_count`      | —    | 每帧 sample frame 数 F（必填，来自 server）          |
| `concealment`      | off  | Phase 2 PCM concealment（`enabled` + `max_slots=3`） |
| `splice`           | off  | 修正拼接 crossfade（见下；产品默认开） |
| `target`           | 0.60 | 恢复目标 / 稳态中心                                  |
| `normal_low`       | 0.35 | normal 下界                                          |
| `normal_high`      | 0.80 | normal 上界                                          |
| `warning_low`      | 0.20 | warning / deadline 下分界                            |
| `warning_high`     | 0.90 | warning / deadline 上分界                            |
| `startup_level`    | 0.50 | 启动 pre-roll 锚定水位                               |
| `step` / `step_fn` | —    | warning 区步长参数与步长函数（函数指针，无状态）     |

## 修正拼接 crossfade（splice）

Warning 修正（FILL 重播 / DROP 跳槽）是整包硬拼接（3.646ms @F=175/48kHz），音乐里即咔哒/小断音；
concealment 只盖缺帧，不管修正拼接点。splice 只修拼接听感，不动时间轴：

- DROP 着陆、FILL 重播开头、conceal 启动、进静音四处不连续点 arm，
  后续输出前 N 帧（默认 64 ≈ 1.33ms）从"最后一个已播采样"线性淡出。
  起点精确等于已播值（与耳朵连续），终点精确等于新内容，相同值混合恒等。
- 缓冲全预分配（2 × 一采样帧），pull 热路径零分配零锁；未启用时零开销。

组件默认关（v1 硬拼接，老测试行为不变），`ClientRuntime` 默认开、无 CLI 旋钮。
单测 `jitter_buffer_splice_test.cpp` 用 S16LE + 小 xfade 做精确整数断言。

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

`pull()` 禁止：mutex、堆分配、系统调用、阻塞等待、同步日志。`AQUA_JB_RUNTIME_THREAD_DEBUG_LOG` 是开发期开关，开启会破坏 RT
契约，不可用于生产构建。

## 测试

`tests/audio/` 下：`jitter_buffer_test.cpp`（基础与边界）、`jitter_buffer_boundary_test.cpp`（水位边界）、
`jitter_buffer_recovery_regression_test.cpp`（排空后重新入帧不静默饿死等回归）、
`jitter_buffer_splice_test.cpp`（splice crossfade 精确混合 + 纯净区逐字一致）、
`jitter_estimator_test.cpp`（JitterEstimator 单测）、`target_controller_test.cpp`（TargetController 单测）、
`jitter_buffer_concealment_test.cpp`（Phase 2 concealment + 欠载/迟到计数）。
