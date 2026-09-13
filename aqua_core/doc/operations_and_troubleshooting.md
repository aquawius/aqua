# 运维与故障排查

## 1. Client 连不上

先看：

```text
gRPC connect
ConnectResponse valid?
UDP remote configured?
UDP receive started?
Heartbeat ack count/misses
```

若 gRPC 成功、Heartbeat 连续 miss=3：问题通常在 UDP 路径、防火墙、advertised address 或目标 endpoint，而不是 JitterBuffer。

## 2. Client 有连接但没声音

按顺序看：

1. `playback_running`
2. `audio_error`
3. Server `frames_broadcast`
4. Client UDP `rx_packets/rx_bytes`
5. `udp_audio_payload_mismatches`
6. `jb_push_accepted`
7. `jb_pull_silence_frames`
8. Jitter `water/used/capacity`

### 常见解释

- `rx=0`：先查 UDP。
- `rx>0` 但 `push_ok=0`：查 payload geometry/sequence/slot collision。
- `push_ok>0` 但长期 `silence_frames≈pull_frames`：查 pre-roll、水位、缺帧或 playback callback。
- `water` 长期 > 90%：发送快于消费或时间线失配，观察 Drop/reanchor。
- `water` 长期 < 30%：网络供给不足或播放消费快于接收，观察 Fill/缺帧静音。

## 3. 高负载下丢包

Server 重点观察：

- capture starved
- queue dropped
- dispatcher encode/dispatch failures
- UDP tx dropped / enqueue failures

Capture RT 到 network worker 中间只有有界 queue；它满时丢最新 frame，这是有意的 backpressure policy。不要因此把 Server
queue 当作网络 jitter buffer。

## 4. JitterBuffer 问题

优先打开 Debug，再短时间开启 RT debug log。重点关注：

```text
pre-roll anchor
FILL enter/complete
DROP enter/complete
REANCHOR request/apply/cancel
slot busy / late / sanity reject
```

长期记录 RT debug 日志会改变 timing，不应作为性能基线。

## 5. 地址问题

IPv6 一律使用：

```text
[addr]:port
```

内部 `parse_ip_address()` 只接受 IP literal，不解析主机名。CLI client 的 `--server-ip` 明确拒绝 unspecified address 和非
IP 主机名；CLI server 的 `--server-ip` 与 `--udp-advertise-ip` 允许 wildcard（server 要监听所有网卡），但通告 wildcard 时
client 会 回退到 gRPC 连接所用的地址。

## 6. 设备切换

设备失效 **不再**终止进程。排查时先看切换维度，再看音频维度：

```text
Server：capture_switch.state / route / last_outcome / last_switch_error
Client：playback_state / route_mode / switch_outcome / switch_error
```

| 现象                              | 含义与处理                                                                 |
|-----------------------------------|----------------------------------------------------------------------------|
| `switch=switching` 长时间不变     | 事务卡在设备打开；看日志中哪个候选在失败                                   |
| `last_switch=rolled_back`         | 目标设备不可用，已回到先前的实际设备（临时降级，用户意图未变）             |
| `last_switch=fell_back_to_system` | 目标与回滚都失败，落到了系统默认                                           |
| `switch=fatal`                    | 候选链耗尽或 10s 内超过 3 次自动 restart；会话会被终止，看最后一个错误原因 |
| 频繁切换（每次间隔 < 10s）        | 设备插拔风暴；达到预算上限后会 Fatal，属预期保护                           |
| 切换后 client 短暂无声            | 预期：server 切换是 packet gap，由 client JitterBuffer 的饥饿路径吸收      |

日志关键字（Debug 级）：

```text
capture device error ..., switch pending          错误已上报，等 control tick
CaptureManager switch begin / completed           事务开始与结果（含候选数）
system default device changed ... following       跟随系统默认设备变化
capture switch fatal (fallback chain exhausted)   链耗尽，进程退出
```

不要用"静音"或"低能量"判断设备故障：loopback 在没有 render client 时静默并产出合成静音是合法稳态。只有
`DeviceDisconnected` 与设备列表变化会触发切换。

## 7. 与 Degraded 的区分

- **切换中 / 已切换**：会话保持 `Running`，`last_switch_error` 记录原因（成功后清零错误通道）。
- **`Degraded`**：不可自愈的终止条件——非设备的后端错误，或切换 Fatal。CLI 下一 tick 停止进程，退出码为 0（脚本无法据此
  区分"正常退出"与"故障退出"，排障要看日志末尾）。

## 8. 延迟 / 抖动排障（低延迟场景专用）

这一节的判据全部来自实机对照（同一台 server 二进制，**LAN 0 次 stall / Wi-Fi 每分钟上百次 stall**），
按"先分责、再调参"的顺序组织。**先证明问题属于哪一侧，再动参数**——否则会拿 JB 参数去补
一个根本不是抖动的问题（本项目六轮实测里，真正影响听感的五次都不是 JB 控制律造成的）。

### 8.1 第一步：用 LAN 对照把"链路"和"代码"分开

同一对机器、同一份二进制，分别走有线与无线各跑 60 秒（**中途不要重启**，启动头 10 秒的
湍流不算稳态）。

| 结果 | 结论 | 去向 |
|---|---|---|
| LAN 也有 stall | 发送端或接收端代码问题 | 8.2（看 server 侧计数） |
| LAN 0 stall，Wi-Fi 有 stall | **链路层属性**（AP / 网卡 / 驱动 / 蓝牙共存 / 省电） | 8.4（只能靠缓冲深度换） |
| 两边都干净但听感差 | 不是网络问题 | 看 playback xrun、capture starved、格式协商 |

### 8.2 第二步：server 侧三件事定发送端责任

1. `pacing enabled: ... wait_mode=high_res_timer` —— 没出现 `high_res_timer` 说明退化到
   `timeBeginPeriod(1)`，3.6ms 的 pacing 在老系统上会被 15.6ms 粒度吃掉。
2. 停止行 `paced_sends` 增速 ≈ 发包率（274 包/s @48kHz·F=175）；明显偏低 = 排表被降速。
3. **`catchup_drains` 稳态应为 0**：>0 表示 pacing worker 被调度饿死（CPU 争用、笔记本省电、
   线程优先级），积压只能靠追赶路径排空——这是发送端把自己的突发搬到接收端。

### 8.3 第三步：client 侧两项指标定接收端责任

- **稳态 `jit_ms` > 1ms** = 到达间隔不平滑（发送端 burst 或链路整形）。pacing 真正生效后应在
  0.2~0.6ms 量级；曾出现过"稳定在 3.2ms"的形态，根因是发送端每 10ms 打出**成对**包。
- **`stall` + `stall_peak_ms`** = 时间断流（>5 个包周期）。看它的**分位数**而不是最大值：
  例如实测 p50 20.4ms / p90 24.5ms / p99 29.2ms / max 41.7ms —— 用 p99 选缓冲深度比用 max 经济得多。

### 8.4 延迟预算与唯一主旋钮

```text
端到端 ≈ 采集设备周期(10ms) + pacing 排队(≈半个 packet)
       + 网络单程 + JB target + 回放设备周期
```

其中**只有 JB target 是可调的**，且它有一个压不下去的硬下限（几何地板）：

```text
target ≥ 几何地板 = ceil(回放 callback 帧数 / F) + 1
```

Windows WASAPI shared 默认 480 帧/10ms、F=175 → 地板 4 槽 ≈ **14.6ms**。

在无线链路上要覆盖 p99 空隙，只能把 target 抬到能盖住它的高度：`--jb-min-target`
（CLI）或 Android 的 `WIFI_SMOOTH` 档。**换算方法**：

```text
槽数 = 目标覆盖的空隙时长 / (F / sample_rate)
例：p99 = 29.2ms，packet = 3.646ms → 9 槽（32.8ms）
```

经验档位（同一条 Wi-Fi 实测）：`6` 槽=21.9ms（低延迟极限，偶有欠载）、`9` 槽=32.8ms（覆盖 p99）、
`12` 槽=43.8ms（覆盖更差环境）。**再往下压只会把抖动换成欠载/concealment**，不是优化。

### 8.5 别忘了"测量装置"本身

- debug 级别下，老二进制的 1s 诊断行会同步写控制台并阻塞 io_context（新版本已改为异步日志 +
  独立线程）——**看到 1Hz 周期性空隙时先怀疑这个**。
- 长时间开 RT debug 日志会改变 timing，不能作为性能基线（§4 已述）。
- 测稳态要连跑 ≥60s 再下结论：启动头 10 秒 target 要从地板爬坡、回放要起流，那段湍流是结构性的。
