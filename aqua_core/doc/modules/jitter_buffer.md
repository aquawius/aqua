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
6. 时间线明显跳变时安全 reanchor。

它不是"按毫秒睡眠"的缓冲：容量与调整动作的基本单位都是 **slot**。

## 几何

```text
N = capacity_slots           默认 30（CLI --jitter-slots，范围 4..4096）
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
```

这样后端不会因为 callback 未填满而重复播放上一次缓冲的残留数据。

`JitterBufferPullResult{frames_filled, silence_frames, skipped_slots}` 分别对应：实际填充帧数、其中静音帧数、本次跳过的
槽数（Drop）。

### 配置

| 字段            | 默认 | 含义                                        |
|-----------------|------|-----------------------------------------------|
| `capacity_slots`| 30   | 环形槽数 N（下限 4）                          |
| `format`        | —    | 权威 PCM 格式（必填）                         |
| `frame_count`   | —    | 每帧 sample frame 数 F（必填，来自 server）   |
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
`jitter_buffer_recovery_regression_test.cpp`（排空后重新入帧不静默饿死等回归）。
