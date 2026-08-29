# 模块：AudioPacketizer

## 文件

- `aqua_core/include/aqua/audio/packetizer/audio_packetizer.h`
- `aqua_core/src/audio/packetizer/audio_packetizer.cpp`

## 职责

`AudioPacketizer` 位于 Server 的 realtime capture callback 与网络发送队列之间。它把输入侧的**可变长度、非拥有** `AudioBlock` 转换成固定 `frame_count` 的 `AudioFrame`。它只解决“分块与序号”，不负责 UDP 编码、不负责 Session、不负责丢包重传。

## 为什么需要它

WASAPI/PipeWire 等采集后端给出的 callback block 长度不保证等于网络槽尺寸。如果直接把一个 block 当作网络帧，会产生可变 payload；当前协议则要求一个 Audio datagram 对应一个固定几何的 AudioFrame。因此 Packetizer 必须保存尚未凑满一个 slot 的尾部。

## 输入/输出契约

输入必须满足：

1. `AudioBlock.format` 有效；
2. block 的字节数是 `frame_bytes` 的整数倍；
3. Packetizer 已按 Server 最终格式和 `frame_count` 构造。

输出满足：

- 每个输出帧恰好包含固定 `frame_count` 个 sample frames；
- sequence 每输出一个完整帧递增一次；
- 最后不足一个帧的输入不会丢弃，而留在内部 pending buffer；
- sink 在 callback 内被同步调用，因此 sink 必须 noexcept、非阻塞、无分配。

## 数据流

```text
Capture callback
      │ AudioBlock(span)
      ▼
AudioPacketizer::push
      │
      ├── pending 旧尾部
      ├── 新输入
      │
      ├── 满 frame_count ──► AudioFrameSink
      │                         │
      │                         └── FrameQueue::push
      │
      └── 不足 frame_count ──► pending
```

## sequence 语义

sequence 对应**完整 PCM sample-frame slot**，不是 UDP datagram 次数，也不是 byte offset。Server 每产生一个完整 AudioFrame，sequence 加一。Client JitterBuffer 使用该序号建立播放时间线。

## 边界行为

- 一个输入 block 可以产生 0、1 或多个 AudioFrame。
- 一个 AudioFrame 可以由相邻多个 capture block 拼成。
- `push()` 的输入若未对齐，直接拒绝并记录 `input_unaligned`，不会尝试补零或隐式格式转换。
- pending 的容量在构造时确定；运行期不应因为输入 block 大小变化而分配。

## 并发

Packetizer 本身属于 capture realtime side，只允许一个调用者按顺序 `push()`。它不内部加锁。sink 调用仍处于 realtime callback 上下文。

## 与网络层的边界

Packetizer 不知道：

- SessionManager；
- UDP peer；
- HELLO/ACK；
- gRPC；
- MTU。

它只产生满足协议几何要求的 AudioFrame。MTU 是否可容纳该帧由 Runtime 在启动时验证。

## 失败与限制

当前设计没有“分片一个 AudioFrame”的能力。一个最终 AudioFrame 必须完整装进单个 UDP Audio datagram；因此 `frame_count * frame_bytes` 必须不超过协议允许的 `UDP_AUDIO_PAYLOAD_BYTES`。

## 测试重点

应覆盖：单 block、多 block 拼帧、一次 push 产生多个帧、尾部 pending、unaligned 输入、sequence 连续性以及 sink 不消费时 Packetizer 自身仍保持确定性。
