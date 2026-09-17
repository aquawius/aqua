# 模块：Server Audio Path

## 组件

```text
AudioCapture（由 CaptureManager 持有，故障时可重建端点）
   ↓
AudioPacketizer
   ↓
AudioFrameQueue
   ↓
AudioNetworkDispatcher
   ↓
UdpServer
```

采集端点切换不会重建右侧任何组件：packetizer、队列、dispatcher、UDP 与会话都原地保留，切换期间只是没有生产者。

## AudioPacketizer

构造期分配一个 `pending_`，大小恰好 `F × frame_bytes`。每次 `push(span<const byte> pcm, sink)`（输入是自消耗的 span 游标，
`AudioBlock` 只出现在 capture 回调边界）：

1. 计入 `input_blocks` / `input_bytes`（ **在校验之前**，被拒的块也计入）；
2. 校验 block 按 sample frame 对齐，未对齐则记 `rejected_unaligned_blocks` 并丢弃；
3. 将输入按 pending 剩余空间拷贝；
4. 填满后生成一个 AudioFrame；
5. 同步调用 sink；
6. sequence++；
7. pending_size=0。

因此一次 WASAPI callback 可以生成 0、1 或多个 AudioFrame，也可能留下一个未完整 slot 的尾巴。尾巴留在 `pending_`，下一次继续填。

## AudioFrameQueue

固定容量 SPSC：

```text
producer = capture RT
consumer = dispatcher worker
```

满时 **drop newest**。每个 queue slot 自带 PCM storage，所以 consumer callback 返回前 producer 不能复用该 slot。

wake generation 的作用只是唤醒休眠 worker，不承担 correctness。consumer 每次真正运行都重新读取 head/tail。

## Dispatcher 与发包 pacing（低延迟链路的关键杠杆）

Dispatcher 唯一跨越 audio -> network domain。worker 从 queue 取 AudioFrame 后编码成共享
`std::vector<byte>`，再交给 `UdpServer::broadcast()`。

这样一个 frame 只 encode 一次，所有 connected sessions 共用同一份 immutable datagram；避免每个 client 都重新序列化。

### 为什么必须 pacing

采集是 **按 callback 成串产出**的：WASAPI shared 模式 10ms 回调一次，一次交付 2~3 个已打包帧 （48kHz / stereo / F32 下
F=175，packet=3.646ms）。若 dispatcher 收到通知就把队列清空， 链路上看到的就是"每 10ms 一串、串内间隔≈0"的锯齿：

- 接收端 RFC 3550 的 **均值型**抖动估计 J 被这种 **确定性 burst** 撑大（实测 3~4ms）；
- J 越大 → 尾部 P99 越大 → 自适应 target 越高 → 端到端延迟越高；
- 串内包挤在一起还会把 AP/交换机队列打深，反过来放大下一跳抖动。

pacing 的目标很朴素： **让到达间隔回到均匀的 packet 周期**。发送端为此付出约半个 packet 周期的 平均排队（ ~1.8ms），换掉接收端
10~15ms 的缓冲加深 —— 这是整条链路里性价比最高的一笔交易。

### 四条实现规则（每一条都是实测踩出来的）

1. **绝对时刻表**：`next_send += interval`， **不是** `now + interval`。相对排表会把每拍
   ~1ms 的唤醒延迟累积成系统性降速（实测 274→220 包/s、队列稳定积压 4 槽、追平逻辑被反复打到）；
   绝对排表下小于一个间隔的唤醒延迟不累积，发送速率恒定等于生产率。
2. **高精度等待（Windows 必需）**：默认定时器粒度 15.6ms 会把 3.6ms 的睡眠直接量化掉， pacing 退化成"睡 15.6ms
   再一次性全发"——比不做 pacing 还糟。`PaceWaiter` 优先
   `CREATE_WAITABLE_TIMER_HIGH_RESOLUTION`（Win10 1809+，亚毫秒、无系统级副作用）， 老系统退化 `timeBeginPeriod(1)`。启动日志
   `pacing enabled: interval=... wait_mode=high_res_timer` 可确认走了哪条路。
3. **队列睡空后重新锚定**：队列空 → worker 进 cv 等待 → 醒来时 `next_send = now`。 睡眠期间没有产出可发的帧，那段"欠账"不是真
   backlog；不锚定就会在每个 capture block 开头背靠背补发两包（实测接收端 J 被系统性抬到 ~3.2ms、target 高出地板 1~2 槽）。
4. **追赶 = 加速排空，不是一次性清空**：积压 ≥ `CATCHUP_DEPTH` 时按
   `interval / SPEEDUP` 逐包发（净排空速率 = (SPEEDUP−1)×正常速率）。整批 `drain()`
   只是把突发从发送端搬到接收端（8 个包挤在 <1ms 内到达，接收端抖动估计瞬间被打高）， 与 pacing 的意图自相矛盾。

### 参数与不变量

| 量                                    | 默认值                                      | 约束 / 为什么                                               |
|---------------------------------------|---------------------------------------------|-------------------------------------------------------------|
| pacing interval                       | `F × 1e9 / sample_rate` ns（= 3 645 833ns） | 由 F 与采样率派生，**不是配置项**                           |
| `DISPATCH_PACING_CATCHUP_DEPTH_SLOTS` | 8（≈29ms 积压）                             | 必须明显大于自然摆动深度 0..5（阈值 4 实测每秒触发 ~20 次） |
| `DISPATCH_PACING_CATCHUP_SPEEDUP`     | 3                                           | 净排空 2× 生产率，饿死后能追上又不成突发                    |
| `DISPATCH_PACING_MAX_CATCHUP_SENDS`   | 2                                           | 单拍欠账补发上限                                            |
| `MIN_AUDIO_QUEUE_CAPACITY_SLOTS`      | 9（= 深度+1）                               | 队列容量必须 > 追赶深度，否则队列先丢最新帧、追赶永远走不到 |

### 健康判据

- 停止日志里 `paced_sends` 的增速 ≈ 发包率（274 包/s @48k·F=175）， **`catchup_drains` 稳态应为 0**；
  `catchup_drains > 0` 表示 worker 被调度饿死过（查 CPU 争用、笔记本省电、线程优先级）。
- client 侧稳态 `jit_ms` 应 < 1ms（pacing 生效前是 3~4ms），这是判断 pacing 是否真的生效的最灵敏指标。
- client 侧 `stall` 计数与 server 侧 `catchup_drains` 应同涨同落；只有 client 侧涨 = 链路问题（不是发送端）。

## 丢弃点

Server audio path 有三个明确的 drop 位置，语义各不相同，排障时必须分开看：

| # | 位置                     | 策略        | 计数器                                             |
|---|--------------------------|-------------|----------------------------------------------------|
| 1 | packetizer 输入未对齐    | 整块拒绝    | `packetizer.rejected_unaligned_blocks`             |
| 2 | `AudioFrameQueue` 满     | drop newest | `queue.dropped_frames`                             |
| 3 | UDP transport 待发队列满 | drop oldest | `net.transport.tx_dropped` / `tx_enqueue_failures` |

注意诊断快照里的 `dispatcher.dropped_frames` 实际转发的是 `AudioFrameQueue` 的丢弃数（dispatcher 自身没有丢弃计数器），
不要把它当成" dispatcher 丢了帧"。
