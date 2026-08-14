# DiagnosticsManager 诊断模块

`DiagnosticsManager` 采集客户端运行时网络、缓冲、调度、时钟漂移等指标，周期性输出 DEBUG 日志并暴露结构化快照供外部查询。

---

## 日志格式

`--log-level debug` 下，client 主线程每 ~5s 调用一次 `collect_and_log()`，输出一行：

```
Client diag: RTT={:.1f}ms jitter={:.2f}ms loss={}/{:.3f}% dup={} late={} dmiss={}
JB[{:.0f}/{:.0f}/{:.0f}/{:.0f}ms] RB[{:.0f}/{:.0f}/{:.0f}/{:.0f}ms]
underrun={} slope_s={:.1f} slope_l={:.1f} e2e={:.1f}ms drift={:.1f}ppm
rx_bytes={} acks={}
```

示例（本机回环）：

```
Client diag: RTT=0.2ms jitter=4.53ms loss=0/0.000% dup=0 late=0 dmiss=54705
JB[21/25/6/42ms] RB[19/18/0/29ms] underrun=0 slope_s=179.1 slope_l=-4.7 e2e=40.0ms drift=-199.2ppm
rx_bytes=12345678 acks=42
```

---

## 字段说明

### 网络层

| 字段 | 含义 |
|:-----|:-----|
| **RTT** | HELLO→HELLO_ACK 往返时延（ms），EWMA 平滑。反映链路 ping 级延迟 |
| **jitter** | 端到端到达间隔抖动（ms），RFC 3550 EWMA。包含网络抖动 + 发送端调度抖动 |
| **loss** | `loss=N/p%`：N = `lost + late` 会话累计值；p = `(lost+late)/received` 百分比 |
| **dup** | 重复包数（JB 归类为 duplicate），会话累计 |
| **late** | 超过播放 deadline 才到达、被丢弃的包，会话累计 |
| **rx_bytes** | 收到的音频总字节数（payload only），会话累计 |
| **acks** | 收到的 HELLO_ACK 总数，会话累计 |

**丢包拆分**：`loss` 的 N 是 `lost + late` 之和。`late` 高而 `lost` 低 → 网络抖动/乱序；`lost` 高 → 真丢包或拥塞。

### JitterBuffer 与 RingBuffer 水位

| 字段 | 含义 |
|:-----|:-----|
| **JB[cur/avg/min/max]** | JitterBuffer 当前/平均/最小/最大水位（ms） |
| **RB[cur/avg/min/max]** | 播放 RingBuffer 水位（ms）。容量上限 = `PLAYBACK_RINGBUFFER_SIZE`（16KB ≈ 42.7ms） |

JB 水位高于配置目标属正常：Windows 定时器粒度（~15.6ms）下 JB 批量 pop，需预先缓冲约一个定时器周期 + 抖动量的 future 包。

### 调度与漂移

| 字段 | 含义 |
|:-----|:-----|
| **dmiss** | deadline miss 计数：JB 定时器比 deadline 延迟超过 1 个包（`AUDIO_PACKET_MS`=3ms）的次数 |
| **underrun** | WASAPI 回读不及时次数（读少于请求量，补静音） |
| **slope_s** | RingBuffer 5s 窗口占用斜率（samples/s），反映短期调度/网络波动 |
| **slope_l** | RingBuffer 60s 窗口占用斜率（samples/s），反映缓冲量缓慢增减趋势 |

**dmiss 极高是预期行为**：定时器每 ~15.6ms 触发一次，每次批量 pop 跨越多个 3ms deadline，几乎每次都超 1 个包。它衡量"定时器不精确度"，由批量 pop 机制兜底，不代表故障。

### e2e（端到端延迟）

`e2e = JB 当前水位 + RB 当前水位`（ms），即当前缓冲量意义上的延迟。

- 无需时间同步，语义直白：此刻收到的音频还要经过多少缓冲才被播放
- 回环下 ≈ JB(~20ms) + RB(~20ms) ≈ 40ms 量级

### drift（时钟漂移）

`drift = (server 发送速率 / 客户端播放速率 − 1) × 1e6`（ppm）。

- **server 发送速率**：对最近 10s 的 `(到达时间, sample_position)` 做线性回归，斜率即稳定发送速率（帧/秒）
- **客户端播放速率**：对最近 10s 的 `(时间, 累计播放帧数)` 做同样回归
- 两者都用长窗口回归平均掉逐包抖动和包边界相位，得到单一符号的稳定值
- 正 = server 快于播放（JB 渐满，可能溢出）；负 = server 慢于播放（JB 渐空，可能欠载）
- 回环下应接近 0（±几百 ppm）

---

## 架构

### 线程模型

| 调用者 | 方法 | 线程 |
|:------|:-----|:-----|
| UDP recv 回调 | `record_packet_arrival` | io_context 线程 |
| HELLO 发送 | `record_hello_sent` | io_context 线程 |
| HELLO_ACK 接收 | `record_hello_ack_received` | io_context 线程 |
| WASAPI 播放回调 | `record_underrun` | 播放线程 |
| JB 定时器回调 | `record_deadline_miss` | io_context 线程 |
| 主循环（~50ms） | `record_rb_occupancy` | 主线程 |
| 主循环（~5s） | `collect_and_log` | 主线程 |

跨线程共享数据用 `std::atomic`（relaxed）或 `std::mutex` 保护：

- 计数器（`underruns_`、`deadline_misses_`、`recv_audio_bytes_`、`recv_hello_acks_`）：relaxed atomic
- RTT / jitter：relaxed atomic，容忍读到旧值
- `arrival_history_`：由 `arrival_mutex_` 保护（io_context 写、主线程读）
- `last_snapshot_`：由 `snapshot_mutex_` 保护

### 采样与回归

`record_rb_occupancy()` 以 ~50ms 高频采样 RingBuffer 占用和播放进度，分别存入：

- `short_window_`（5s）→ `slope_s`
- `long_window_`（60s）→ `slope_l`
- `played_history_`（10s）→ 客户端播放速率回归

`collect_and_log()` 以 ~5s 低频采集全量指标，从 `arrival_history_`（io_context 线程填充）读取 server 发送速率，计算 drift 并输出日志。

两频率解耦是必要的：若仅在 `collect_and_log` 中采样，5s 窗口只有 1-2 个样本点，线性回归无意义。

### sample_position 回绕处理

`record_packet_arrival` 用 `int32_t` 差值计算 `sample_position` 增量，正确处理 `uint32_t` 回绕，并累积到 `arrival_pos_accum_` 供回归使用，避免回绕导致回归跳变。

---

## Snapshot 结构

`snapshot()` 返回 `Snapshot` 结构体，包含上述所有字段，供外部（如未来 UI）查询当前诊断状态。调用线程安全。
