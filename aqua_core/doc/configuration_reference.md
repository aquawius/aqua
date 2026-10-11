# 配置参考

本文列出所有默认值与限制。常量的权威定义位置：

- `aqua_core/include/aqua/runtime/runtime_config.h`（server 控制面绑定地址、逐族数据面绑定地址与 server 音频路径）
- `aqua_core/include/aqua/runtime/runtime_state.h`（control poll）
- `aqua_core/include/aqua/net/udp/udp_config.h`（UDP 与 session）
- `aqua_core/include/aqua/net/grpc/grpc_config.h`（gRPC，含默认端口与 client 名）
- `aqua_core/include/aqua/diagnostics/diagnostics_config.h`（诊断节奏）
- `aqua_core/include/aqua/audio/audio_format.h`（格式上限）
- `aqua_core/include/aqua/audio/buffer/buffer_config.h`（Buffer 组件全部默认值与策略常量）
- `aqua_core/include/aqua/runtime/client_runtime.h`（采集/回放事件窗口等运行时常量）
- `aqua_core/include/aqua/audio/playback/audio_playback_config.h`（回放帧粒度）
- `aqua_core/include/aqua/audio/capture/audio_capture_config.h`（采集帧粒度）

Buffer 相关数值的 **语义与推导**分别见 `jitter_buffer_control_design.md`（决策层）与 `buffer_design.md`（执行层）； 日志点位见
`modules/observability.md`。本文是唯一的数值速查表，其它文档只引用不复述。

## 1. 网络与会话

| 常量                                     |            值 | 定义位置                                                  |
|------------------------------------------|--------------:|-----------------------------------------------------------|
| `DEFAULT_RPC_PORT`                       |         50051 | `grpc_config.h`                                           |
| `DEFAULT_UDP_PORT`                       |         50000 | `udp_config.h`                                            |
| `DEFAULT_BIND_IP`                        |     `0.0.0.0` | `runtime_config.h`（**控制面**/gRPC 默认绑定地址）        |
| `DEFAULT_UDP_IPV4`                       |     `0.0.0.0` | `runtime_config.h`（IPv4 数据面默认绑定地址）             |
| `DEFAULT_UDP_IPV6`                       |          `::` | `runtime_config.h`（IPv6 数据面默认绑定，`v6_only`）      |
| `DEFAULT_CLIENT_NAME`                    | `aqua-client` | `grpc_config.h`                                           |
| `UDP_RECV_BUFFER_BYTES`                  |         65536 | `udp_config.h`                                            |
| `UDP_SEND_BUFFER_BYTES`                  |         65536 | `udp_config.h`                                            |
| `UDP_AUDIO_PAYLOAD_BYTES`                |          1400 | `udp_config.h`（IPv6 1440 再让 40B 隧道/封装余量）        |
| `UDP_AUDIO_MAX_PACKET_MS`                |         5.0ms | `udp_config.h`（auto-F 包时长上限）                       |
| `UDP_AUDIO_MIN_PACKET_MS`                |         0.5ms | `udp_config.h`（auto-F 包时长下限，≤2000 包/s）           |
| `UDP_MAX_QUEUED_DATAGRAMS`               |            64 | `udp_config.h`                                            |
| `SESSION_TIMEOUT`                        |       5000 ms | `udp_config.h`（只看 proto Keepalive 刷新的 last_seen）   |
| `SESSION_REAP_INTERVAL`                  |        200 ms | `udp_config.h`                                            |
| `HEARTBEAT_HANDSHAKE_INTERVAL`           |       1000 ms | `udp_config.h`（握手期 heartbeat 节奏）                   |
| `HEARTBEAT_INTERVAL`                     |       1000 ms | `udp_config.h`（association 建立后）                      |
| `HEARTBEAT_HANDSHAKE_ACK_MISS_THRESHOLD` |             3 | `udp_config.h`（握手期：连续 3 周期无 ACK 即建连失败）    |
| `HEARTBEAT_ACK_MISS_THRESHOLD`           |             5 | `udp_config.h`（稳态：连续 5 周期无 ACK 即路径死亡）      |
| `GRPC_CONNECT_DEADLINE`                  |       3000 ms | `grpc_config.h`                                           |
| `GRPC_DISCONNECT_DEADLINE`               |       1000 ms | `grpc_config.h`                                           |
| `GRPC_MAX_CLIENT_NAME_BYTES`             |           128 | `grpc_config.h`                                           |
| `GRPC_KEEPALIVE_INTERVAL`                |       1000 ms | `grpc_config.h`（proto 探活节奏）                         |
| `GRPC_KEEPALIVE_DEADLINE`                |        800 ms | `grpc_config.h`（单次超时；必须 < interval）              |
| `GRPC_KEEPALIVE_MISS_THRESHOLD`          |             5 | `grpc_config.h`（传输连续失败阈值；SessionGone 立即上报） |

## 2. 音频几何与缓冲

| 常量                                     |     值 | 说明                                                |
|------------------------------------------|-------:|-----------------------------------------------------|
| `JB_DEFAULT_CAPACITY_SLOTS`              |     30 | client 抖动缓冲槽数（`--jb-capacity`）              |
| `JB_MIN_CAPACITY_SLOTS`                  |      4 | 下限：低于 4 时五个水位带无法保持严格序             |
| `JB_MAX_CAPACITY_SLOTS`                  |    512 | 上限（reanchor O(N) 扫描的 RT 护栏）                |
| `DEFAULT_AUDIO_QUEUE_CAPACITY_SLOTS`     |     48 | server 交接队列槽数（`--audio-queue-capacity`）     |
| `MIN_AUDIO_QUEUE_CAPACITY_SLOTS`         |      9 | 交接队列下限：必须 > 追赶深度（`runtime_config.h`） |
| `DISPATCH_PACING_CATCHUP_DEPTH_SLOTS`    |      8 | 发包 pacing 追赶**进入**高水位（积压达此值进入追赶）        |
| `DISPATCH_PACING_CATCHUP_LOW_WATER_SLOTS` |    2 | 追赶**退出**低水位。缺它队列会永久停在深度−1（见 `server_audio_path.md`） |
| `DISPATCH_PACING_CATCHUP_SPEEDUP`        |      3 | 追赶期的排空倍速（间隔 = interval/SPEEDUP）        |
| `DISPATCH_PACING_MAX_CATCHUP_SENDS`      |      2 | pacing 绝对时刻表欠账补发上限（`runtime_config.h`） |
| `MAX_AUDIO_QUEUE_CAPACITY_SLOTS`         |   4096 | 上限                                                |
| `MIN_FRAMES_PER_SLOT`                    |     16 | 显式 F 的下限（`udp_config.h`）                     |
| `AUDIO_FORMAT_MAX_CHANNELS`              |     64 | 声道上限                                            |
| `AUDIO_FORMAT_MAX_SAMPLE_RATE`           | 768000 | 采样率上限                                          |
| `AudioPlaybackConfig::frames_per_buffer` |      0 | 0 = 后端自选；WASAPI 取设备最小周期                 |
| `AudioCaptureConfig::frames_per_buffer`  |      0 | INPUT 取最小周期；loopback 得默认周期               |

> 定义位置：JB 三项（容量 / 下限 / 上限）在 `audio/buffer/buffer_config.h`；
> `MIN_FRAMES_PER_SLOT` 在 `udp_config.h`；队列与 pacing 六项在 `runtime_config.h`。
> 两个 `frames_per_buffer` 的逐后端语义（含"取不到更小"的原因）见
> `audio/playback/audio_playback_config.h` 与 `audio/capture/audio_capture_config.h`。

F 的推导（auto-F）：`F = min(floor(UDP_AUDIO_PAYLOAD_BYTES / frame_bytes), floor(sample_rate × UDP_AUDIO_MAX_PACKET_MS))`
——MTU 预算与包时长上限（5ms）取小，使各格式包时长处于同一量级。例：48kHz stereo F32 → 175（3.65ms，MTU 封顶）； 48kHz mono S16 →
240（5ms，时长封顶）；44.1kHz stereo S16 → 220（4.99ms，时长封顶）。 显式 F 需满足 `F >= 16` 且 `F × frame_bytes <= 1400`
，否则启动被拒。 两种情形都再校验 `F / sample_rate >= UDP_AUDIO_MIN_PACKET_MS`（0.5ms）：多声道 + 高位深 + 高采样率 （如 7.1ch
F32 @96/192kHz）在 1400B 预算下只能给出极小的 F，包率会高到 pacing 无法实现、交接队列 按槽计只盛得下几毫秒音频（采集每 10ms
交付数十包，必然持续溢出）。这类格式在 1400B payload 下 无法可靠传输，启动期直接拒绝，而不是静默跑起来丢帧。

## 3. 运行期节奏

| 常量                            |      值 | 说明                                                          |
|---------------------------------|--------:|---------------------------------------------------------------|
| `RUNTIME_CONTROL_POLL_INTERVAL` |  500 ms | control tick（server 切换 / client 恢复与跟随）               |
| `DIAGNOSTICS_SNAPSHOT_INTERVAL` | 1000 ms | 诊断快照与输出（`diagnostics_config.h`）                      |
| 设备事件合并窗口                | 1000 ms | client 侧 `notify_devices_changed` 去抖（`client_runtime.h`） |

## 4. 设备切换

| 项                | 值               | 说明                                           |
|-------------------|------------------|------------------------------------------------|
| 候选链            | 3 层             | `[目标设备, 先前的实际设备, 系统默认]`，去重   |
| 设备丢失 restart 预算 | 10s 窗口内 3 次 | 错误驱动与默认跟随**共享**同一预算，超限 Fatal |
| 路由未稳定（settle）预算 | 5s 窗口内 8 次，最小间隔 200ms | **仅 client**（`PlaybackManager`）。判据三条缺一不可：已认领当前流、backend 已 `!is_running()`、流龄 < 400ms（`kRouteSettleWindow`）。与设备丢失预算**完全独立**；节流命中时不改任何状态直接返回，由 500ms supervision tick 的 silent-death 分支再驱动。capture 侧**无**此预算 |
| client 显式选择   | 不计数并重置**两套**窗口 | 用户手动选择设备（`set_playback_device`） |
| server 手动切换   | 不提供           | server 无运行时切换入口，sticky = CLI 配置     |

## 5. JitterBuffer 策略

> 以下常量的权威定义与取值理由见 `buffer_config.h`（`aqua::config::JB_*`），
> 本节只做速查。

```text
capacity      = client 配置（默认 30 slots）
startup_level = 0.50N    启动 pre-roll 锚定水位（独立于稳态阈值序）
target        = 0.60N
normal_low    = 0.35N
normal_high   = 0.80N
warning_low   = 0.20N
warning_high  = 0.90N
```

warning 区步长：

```text
min_step        = 1
max_step        = max(2, round(0.10N))     # max_step=0 时自动取值
growth          = 2.0
growth interval = 4 次连续 warning evaluation
```

重锚定：

```text
config::JB_MAX_REANCHOR_JUMP_FRAMES     = 100000   超过则判为荒谬并拒绝该帧
config::JB_REANCHOR_MIN_GAP_SLOTS       = 4        缺口达到该值才受理 reanchor 请求
config::JB_REANCHOR_HOLD_STUCK_PULLS    = 5        Hold 无进展时强制应用
```

## 5.1 CLI 暴露的自适应旋钮

完整公式（`margin` 的取 max、`effective_min` 的组成、结构上限）与取值推导见 `jitter_buffer_control_design.md` 第5节。

| CLI                     | 默认 | 常量                                           | 说明                                                                       |
|-------------------------|-----:|------------------------------------------------|----------------------------------------------------------------------------|
| `--jb-min-target`       |    3 | `JB_ADAPTIVE_DEFAULT_MIN_TARGET_SLOTS`         | target 硬下限（槽）；有效下限 = max(本值, 几何地板 + 1)，只能抬高          |
| `--jb-stall-peak-cap`   | 12.0 | `JB_ADAPTIVE_STALL_PEAK_CAP_SLOTS`             | stall 峰值项上限（槽）。**0 = 关闭该项**（margin 只剩尾部分位数）；负值 = 默认 |
| `--jb-stall-decay`      |  5.0 | `JB_ESTIMATOR_STALL_PEAK_DECAY_MS_PER_SEC`     | 峰值衰减（ms/s）="峰值记多久"。**0 = 峰值永久保持**；负值 = 默认           |
| `--jb-stall-threshold`  |  5.0 | `JB_ESTIMATOR_DEFAULT_STALL_THRESHOLD_PACKETS` | stall 门（包周期倍数）。**≤ 0 = 关检测**（裸 RFC 3550）                    |
| `--jb-underrun-penalty` |  1.0 | `JB_ADAPTIVE_UNDERRUN_PENALTY_SLOTS`           | 每次欠载抬升下限（槽）。**0 = 关闭整条反馈闭环**；负值 = 默认              |

后四项的 `0` 是 **合法的极值语义**（关闭对应机制）；**例外是 `--jb-stall-threshold`**——
它的 `≤ 0`（含负值）一律等于「关检测」，没有「退回默认」这一档（同表该行已如实标注）。
除这一项外，其余三项都是「0 = 关闭机制、负值 = 退回默认」。做区间检查会把实验矩阵里的 0 点砍掉。C API 的
zero-init 惯例相反（0 = 默认），见第 6 节。

## 5.2 Buffer 组件内部常量（无 CLI 入口）

这几项只有一个很窄的合理区间，没有 CLI 入口；要改就改 `buffer_config.h` 重编译。

| 常量                                               | 默认 | 说明                                                       |
|----------------------------------------------------|------|------------------------------------------------------------|
| `JB_ADAPTIVE_DEFAULT_INITIAL_TARGET_SLOTS`         | 4    | 起步 target（J 在约 16 包内收敛，随即被自适应拉走）        |
| `JB_ADAPTIVE_STARTUP_MIN_SLOTS`                    | 3    | 起步 pre-roll 水位的绝对下限                               |
| `JB_ADAPTIVE_FALL_RATE_SLOTS_PER_SEC`              | 0.2  | 回落限速（只锁跌）。标定依据见 `buffer_config.h` 该常量注释（大事故挂更久是有意取舍）|
| `JB_ADAPTIVE_RISE_DWELL_MS`                        | 5000 | 涨后锁跌窗口。标定依据见 `buffer_config.h` 该常量注释    |
| `JB_ADAPTIVE_UNDERRUN_PENALTY_MAX_SLOTS`           | 6    | 反馈抬升累计上限（防病态放大）                             |
| `JB_ADAPTIVE_UNDERRUN_PENALTY_DECAY_SLOTS_PER_SEC` | 0.5  | 惩罚回落速率（槽/秒）。**比 FALL_RATE(0.2) 快 2.5 倍**——"坏过一次就多安全一会儿"由 target 侧回落限速实现，见 `known_issue.md` |
| `JB_ADAPTIVE_STALL_PEAK_EXTRA_PACKETS`             | 1.0  | margin 峰值项 = stall峰值/包周期 + 本值，与尾部项取 max     |
| `JB_TAIL_WINDOW_PACKETS`                           | 2048 | 尾部滑动窗口（包，约 7.5s）；滑出即遗忘，涨快跌慢天然不对称 |
| `JB_TAIL_HISTOGRAM_BUCKETS`                        | 64   | 1ms/桶 + 溢出桶；P99 每包 64 桶线性扫                      |
| `JB_TAIL_QUANTILE`                                 | 0.99 | 尾部分位数；均值噪声碰不到它                               |
| `JB_TAIL_MIN_SAMPLES`                              | 128  | 窗内样本不足时 P99 无效（发布 -1），抖动项为 0 落地板       |
| `JB_TAIL_PHASE_MARGIN_PACKETS`                     | 1.0  | 抖动项 = P99/包周期 + 本值；调大则抖动链路 target 整体上浮  |
| `JB_CONCEALMENT_DEFAULT_MAX_SLOTS`                 | 5    | 连续掩盖上限（包），超出转静音（同时是 penalty 可闻口径）   |
| `JB_CONCEAL_FADE_CURVE`                            | 0    | 掩盖淡出曲线：0 = 半功率指数 `2^(-i/2)`（每槽 -3dB，匹配对数响度感知；5 槽总衰减 -12dB）；1 = 等功率余弦。构造期填表，RT 侧只查表 |
| `JB_CONCEAL_ENERGY_EMA_SLOTS`                      | 8    | 能量匹配 context（最近真实音频均方 EMA）的时间常数（槽，≈29ms @F=175）。只衰减、不许放大（防"鬼畜鼓点"） |
| `JB_CONCEAL_ENERGY_MATCH_DEADBAND`                 | 0.81 | 均方比 ≥ 本值时不衰减（±10% RMS ≈ 0.9dB，人耳无感）：稳态内容保持逐字重复，也保住首包增益 1.0 的快路径 |
| `JB_CONCEAL_SILENCE_MS_RATIO`                      | 1e-6 | 近静音判定：last_ms < 满量程² × 本值（≈ -60dBFS）时跳过能量匹配，不对静音段做缩放 |

## 5.3 有线局域网（"极低延迟"）档建议

Core 的出厂默认是**抗抖动优先**的一组值，适合"先不出问题再说"。对纯局域网、延迟优先的部署，
下面三项是收益最大的调整（**全部已有 CLI 入口，不需要重编译**）：

```text
--jb-stall-decay 30~50     # 出厂 5.0 ms/s。target 涨是瞬时的，跌要 16~20s：
                           # 一次 40ms 瞬断 ⇒ +33ms 延迟挂十几秒。对 LAN 而言，
                           # "忘记一个陈旧峰值"几乎无代价（真有事 P99 项会在一个包内
                           # 重新抬 target），而"记住它"每秒都在付延迟。
--jb-stall-peak-cap 4~6    # 出厂 12 槽。一次 40ms 瞬断就把 margin 顶到封顶。
--jb-min-target 3          # 出厂 3（跟随几何地板）。这条通常已是默认，别调高。
```

**为什么 Core 默认不直接改成这套**：这些值同时是 500+ 个 JB 测试的基线，且 CLI 的定位是
"给高级用户的可调面"（AGENT.md §8）。Android App 侧已经用 `ULTRA_LOW` 预设实现了等价档位
（容量 16 / 下限跟随地板 / 峰值余量 4 槽 / 衰减 10ms/s），但它**不是** App 的出厂默认 ——
App 的出厂默认是「Wi-Fi」档（默认用户群是手机 + 家用路由器），纯局域网用户往左拖一档即得本档。

## 5.4 端到端延迟预算（参考值，48kHz / stereo F32 / F=175 ⇒ 包周期 3.646ms）

```text
loopback 采集周期          10.0 ms   平台锁定（LOOPBACK flag 不被 InitializeSharedAudioStream 接受）
packetizer 残留             1.8 ms   可消除，但 auto-F 拿不到采集周期，见 known_issue.md §26
交接队列深度                0~18 ms  通常近空
dispatcher pacing           1.8 ms   均值 0.5 个包周期
网络                       0.15 ms  干净局域网
JitterBuffer target        10.9 ms   最大的可调项（Core 默认 min_target=3）
播放端已排队                7.7 ms   设备侧
                                    ──
                                     ≈ 43 ms（Core 默认）/ ≈ 58 ms（App Wi-Fi 预设 min_target=7）
```

九个环节里六个各贡献 ≥1 个包周期，而三个人为项（残留 + 队列 + pacing）合计 3.6~29ms，
与整个 JB 预算同量级。

## 5.5 结构性约束（不是调优值，不要动）

结构决定、或实测证明"改了会坏"的项。 **有意不作为 CLI 选项暴露**——暴露等于暗示可以乱动。

| 常量                                  |  值 | 为什么不能动                                                                                               |
|---------------------------------------|----:|------------------------------------------------------------------------------------------------------------|
| `JB_ADAPTIVE_TARGET_CAPACITY_RATIO`   | 2/3 | 上 1/3 必须留给抖动吸收。顶穿实测：`busy ≈ 25% rx`、`underrun ≈ 30%`，UDP 零丢包而声音全破                 |
| `JB_MIN_CAPACITY_SLOTS`               |   4 | 低于 4 时五个水位带无法保持严格序，`JitterBuffer::create` 直接拒绝                                         |
| `JB_MAX_CAPACITY_SLOTS`               | 512 | 纯护栏；reanchor 在 RT 线程上是 O(N) 扫描                                                                  |
| `JB_ESTIMATOR_REORDER_WINDOW_PACKETS` |  64 | u64 位图一位一包、零成本；再大要换结构                                                                     |

完整推导与实测证据见 `jitter_buffer_control_design.md` 第11节（ADR-2 / ADR-4）。

## 5.6 日志节奏常量（不影响音频行为）

只影响日志密度，不是调参项。点位与排查用法见 `modules/observability.md`。

| 常量                                  |   值 | 说明                                            |
|---------------------------------------|-----:|-------------------------------------------------|
| `JB_CONTROL_LOG_SUMMARY_INTERVAL_MS`  | 5000 | `TargetController` 稳态摘要的节流周期（ms）     |
| `JB_CONTROL_LOG_REJECT_INTERVAL_MS`   | 1000 | push 拒绝汇总的节流周期（ms；首次不节流）       |
| `JB_CONTROL_LOG_REANCHOR_PROBE_EVERY` |  128 | reanchor 探测日志在"未达断裂缺口"分支的节流分母 |

## 6. Android App 默认值

App 复用第 1–5 节的 Core 默认值，下表是 App 层自有默认。参数经 C API 透传给 `ClientRuntime`，语义与 CLI 一致——
`0` / 空 / `-1` 表示"沿用 Core 默认"。

| 参数             | App 默认              | C API 字段                                    | CLI 等价                | 说明                                                                                                      |
|------------------|-----------------------|-----------------------------------------------|-------------------------|-----------------------------------------------------------------------------------------------------------|
| 服务器 IP        | `192.168.1.100`       | `rpc_ip`                                   | `--rpc-ip`           | 首页可编辑；留空回退 `127.0.0.1`                                                                          |
| RPC 端口         | `50051`               | `rpc_port`                                    | `--rpc-port`            | 1..65535；非法回退 50051                                                                                  |
| 抖动缓冲槽数     | 0（Core 默认 30）     | `jb_capacity_slots`                           | `--jb-capacity`         | 0=默认；显式 4..512（UI 上限 400；低于 4 水位带无法严格排序；512 是 reanchor O(N) 扫描的 RT 护栏）        |
| 自适应 jitter    | 开                    | `jb_fixed_target`（0=开）                     | `--jb-fixed-target`     | 切回既有固定 target/水位                                                                                  |
| PCM concealment  | 开                    | `jb_disable_concealment`（0=开）              | `--jb-no-conceal`       | 缺帧 repeat-last + 短淡出；关=硬静音（同时把 penalty 口径切回逐事件）                                      |
| splice 跨接淡入  | 开                    | 仅 CLI（C API / App 未暴露）（内部 bool）              | `--jb-no-splice`        | 修正拼接 1.33ms 淡入；关=硬拼接，做涂抹 A/B                                                               |
| target 下限      | 3                     | `jb_min_target_slots`（0=默认）               | `--jb-min-target`       | 有效下限 = max(本值, 几何地板+1)；只能抬高，压不到地板以下                                                |
| stall 峰值项上限 | 12.0                  | `jb_stall_peak_cap_slots`（0/负=默认）        | `--jb-stall-peak-cap`   | 0 的"关闭峰值项"极值只在 CLI 提供（zero-init 惯例：0 = 默认）                                             |
| stall 峰值衰减   | 5.0                   | `jb_stall_peak_decay_ms_per_sec`（0/负=默认） | `--jb-stall-decay`      | 0 的"峰值永久保持"极值只在 CLI 提供                                                                       |
| stall 门阈值     | 5.0                   | `jb_stall_threshold_packets`（0/负=默认）     | `--jb-stall-threshold`  | ≤0 的"关检测（裸 RFC 3550）"极值只在 CLI 提供                                                             |
| 欠载惩罚步长     | 1.0                   | `jb_underrun_penalty_slots`（0/负=默认）      | `--jb-underrun-penalty` | 0 的"关闭反馈闭环"极值只在 CLI 提供                                                                       |
| Heartbeat 间隔   | 0（Core 默认 1000ms） | `heartbeat_handshake_interval_ms`             | —                       | 0=默认；UI 0..2000 ms                                                                                     |
| 客户端名称       | 机型名（首跑）        | `client_name`                                 | `--client-name`         | Core 默认 `aqua-client`；App 首跑取设备名，"恢复默认"回 `aqua_android`                                      |
| UDP 端口覆盖     | 空（用 server 通告）  | `udp_force_port`                              | `--udp-force-port`      | NAT / 端口映射场景                                                                                        |
| 日志级别         | -1（Info）            | `log_level`                                   | `--log-level`           | 0..5 = Trace..Fatal                                                                                       |
| playback 帧/回调 | 0（backend 自适应）   | `playback_frames_per_buffer`                  | —                       | WASAPI 取设备最小周期；AAudio 不显式指定容量                                                              |

App 层自有设置（不进入 Core）：

| 设置             | 默认 | 说明                                                                    |
|------------------|------|-------------------------------------------------------------------------|
| 自动重连         | 关   | 播放异常停止 3s 后后台重连（UI 层实现；core 契约为"终态即停"）          |
| 播放时屏幕常亮   | 关   | 播放期间保持屏幕常亮                                                    |
| 允许同时播放     | 关   | 关 = 播放时持有音频焦点；开 = 不申请焦点、与其它 App 共存               |
| 自动切换播放设备 | 开   | 决定连接起步路由：开 = 跟随系统（`RouteIntentOwner::None`），关 = 首流成功后钉住实际落点（`Application`，**非** sticky、设备回归不自动切回） |
| 低延迟模式       | 开   | 对应 AAudio `PERFORMANCE_MODE_LOW_LATENCY`（与 Android 代码默认一致）   |

JB 调优旋钮在 App 高级页的呈现约定（2026-09 起）：

- 数值滑块的 **0 位 = "采用 core 默认值"**，与上表的 zero-init 语义一致（滑块显示"默认 X"而不是 0）；
- 高级页顶部的预设是一条 **延迟梯度滑块**（从左到右 = 延迟从小到大 / 网络从优良到恶劣： 极低延迟 → LAN → Wi-Fi
  （默认） → Wi-Fi 稳定优先 → Wi-Fi 拥挤 → 公网 → 50 ms → 100 ms → 弱网 → 200 ms → 恶劣）， 取值对应
  `jitter_buffer_control_design.md` 第9.1节 的推荐起点，并按
  **低延迟模式**定标 （非低延迟模式设备取数更大，需要更右侧的档）。预设默认只写需要偏离 core
  默认的项、其余留 0 —— 这样 core 默认值演进时预设不会被冻结成一份过期的显式旧值；
  **例外**：参与下面「排序约束」的那几项（容量 / 最低水位 / 断流应对余量 / 记忆衰减 /
  卡顿加固步长）一律**显式固化**，因为 core 调参顺手改掉其中一项就会破坏阶梯单调性
  （2026-10 实例：若 LAN 的断流余量留空跟随 core，core 由 8 升到 12 时它会漂到 12
  而 WIFI 仍是显式的 10 ⇒ 逆序）。理由与当前取值见 `AquaClient.kt` 的 `JbPreset` 头部注释；
- 预设的**单一事实来源是 `AquaClient.kt` 的 `JbPreset` 枚举**，本文档只记录顺序与约束、
  不复写每档数值（复写必漂移——2026-09 那条"漏了「Wi-Fi 稳定优先」、且 50ms/公网 顺序
  与代码相反"的旧序列就是被复写出来的）。改档位后要重新校验下面两条约束；
- **排序约束**：滑块从左到右 = 「越右越抗网络」，因此容量 / 最低水位 / 断流应对余量 /
  卡顿加固步长单调**不减**，断流记忆衰减单调**不增**；另有一条硬约束「**最低水位 ≤ 2/3 × 容量**」
  （否则下限会被结构上限夹住、拖了不生效）。预设的 hint 统一写「下限 …｜上限 …」两个数
  （上限 = 2/3 × 容量），断流余量 / 记忆时长等机制差异写在 summary 里；
- 滑块下方是「自定义配置」开关：关 = 只能用预设滑块（下面的高级滑块由预设整体赋值、 不可拖动）；开 =
  预设滑块锁定、可逐项微调高级滑块。两者互斥，避免"预设说明文字随 拖动变长变短"导致整页跳动；开关与选中的预设都持久化；
- 高级页的「最低水位」与「缓冲容量」 **双向绑定**（1 : 2）：调最低水位时容量自动跟到 至少 2 倍（否则最低水位会被 2/3
  容量上限悄悄夹住、拖了不生效）；调小容量时反向把 最低水位压到容量的一半以内；
- JB 参数是 **连接属性**（JB 构造时确定）：在 App 里改完需重新连接才生效，不会打断当前播放；
- 这些旋钮的"关闭该机制"语义（0 = 关峰值项 / 关断流检测 / 关欠载反馈闭环）只保留在 CLI —— 产品路径不该因为滑块拖到 0
  而静默关掉一层保护。App 侧的 0 一律是"默认值"。

## 7. 不能通过 CLI 修改的协议固定项

- Audio wire header 12 bytes（RTP，大端）；Heartbeat / HeartbeatAck 5 bytes（小端）；
- Audio wire sequence 为 u16（接收端展开成 u64 extended sequence），timestamp 为 u32 （媒体时钟），SSRC 为 u32（每 run
  随机），session id 为 u32；
- 一个 datagram 承载一个完整 AudioFrame；
- Server 一次运行期间 `AudioFormat` 与 F 固定（设备可切换，格式不可变）。
