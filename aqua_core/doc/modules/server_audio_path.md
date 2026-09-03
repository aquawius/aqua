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

构造期分配一个 `pending_`，大小恰好 `F × frame_bytes`。每次 `push(AudioBlock)`：

1. 计入 `input_blocks` / `input_bytes`（**在校验之前**，被拒的块也计入）；
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

## Dispatcher

Dispatcher 唯一跨越 audio -> network domain。worker 从 queue 取 AudioFrame 后编码成共享 `std::vector<byte>`，再交给
`UdpServer::broadcast()`。

这样一个 frame 只 encode 一次，所有 connected sessions 共用同一份 immutable datagram；避免每个 client 都重新序列化。

## 丢弃点

Server audio path 有三个明确的 drop 位置，语义各不相同，排障时必须分开看：

| # | 位置                    | 策略           | 计数器                                     |
|---|-------------------------|----------------|----------------------------------------------|
| 1 | packetizer 输入未对齐   | 整块拒绝       | `packetizer.rejected_unaligned_blocks`       |
| 2 | `AudioFrameQueue` 满    | drop newest    | `queue.dropped_frames`                       |
| 3 | UDP transport 待发队列满 | drop oldest    | `net.transport.tx_dropped` / `tx_enqueue_failures` |

注意诊断快照里的 `dispatcher.dropped_frames` 实际转发的是 `AudioFrameQueue` 的丢弃数（dispatcher 自身没有丢弃计数器），
不要把它当成" dispatcher 丢了帧"。
