# 模块：AudioPacketizer

## 文件

- `aqua_core/include/aqua/audio/packetizer/audio_packetizer.h`
- `aqua_core/src/audio/packetizer/audio_packetizer.cpp`

## 职责

`AudioPacketizer` 位于 Server 的采集 RT 回调与网络发送队列之间：把**变长、非拥有**的 `AudioBlock` 重切成固定
`frame_count` 的 `AudioFrame`。它只解决分块与序号，不负责 UDP 编码、Session、重传，也不感知 MTU（几何由 Runtime 在启动时
校验）。

## 为什么需要它

采集后端一次回调的块长度不保证等于网络槽尺寸（WASAPI 一次事件可能含多个 packet，loopback quiescence 时还会产出合成静音
块）。协议要求一个 Audio datagram 恰好承载一个固定几何的 AudioFrame，因此 packetizer 必须留住尚未凑满一帧的尾部。

## 输入契约

输入是 `std::span<const std::byte>`（`AudioBlock::data`，仅有 `data` 字段，不带格式信息——格式由 packetizer 构造时确定）：

1. 字节数必须是 `frame_bytes` 的整数倍，否则整块拒绝；
2. 空输入直接忽略；
3. packetizer 必须已按会话格式与 `frame_count` 构造（`valid()`）。

## 输出契约

- 每个输出帧恰好 `frame_count` 个 sample frame；
- 每输出一个完整帧 sequence 加一；
- 不足一帧的输入不丢弃，留在内部 pending buffer 等下一次填满；
- sink 在 `push()` 内被同步调用，因此 sink 必须 `noexcept`、非阻塞、不分配（packetizer 用 `static_assert` 强制检查）。

## 数据流

```text
Capture 回调
      │ AudioBlock(span)
      ▼
AudioPacketizer::push
      │
      ├── 拼上 pending 尾部 + 新输入
      │
      ├── 凑满 frame_count ──► sink(AudioFrame) ──► AudioFrameQueue::push
      │                          sequence++
      │
      └── 不足 frame_count ──► 留在 pending
```

一次 `push()` 可以产生 0、1 或多个完整帧；一个完整帧也可以由相邻的多个 block 拼成。

## sequence 语义

sequence 对应**完整的 PCM sample-frame slot**，不是 datagram 次数，也不是字节偏移。它单调递增，由 packetizer 分配，与 capture
流的生命周期无关——因此采集端点切换不会重置 sequence，client 只看到一次 packet gap。

## 计数器

| 计数器                       | 含义                                                        |
|------------------------------|---------------------------------------------------------------|
| `input_blocks`               | 进入 `push()` 的块数（**含随后被拒的未对齐块**）               |
| `input_bytes`                | 进入 `push()` 的字节数（同上，含被拒块）                      |
| `frames_emitted`             | 已输出的完整帧数（= 当前 sequence）                           |
| `rejected_unaligned_blocks`  | 因未按 `frame_bytes` 对齐被拒的块数                           |

对账时要注意 `input_bytes` 包含了被拒块的字节，不能直接用它减去"已发送字节"来推算丢失量。

## 并发与 RT

packetizer 属于采集 RT 侧，只允许一个生产者顺序调用 `push()`，内部不加锁；`pending_` 在构造时一次性分配，运行期不分配。

## 切换时的 pending 残留

`reset()` 用于清空 pending 与计数，契约是"只能在生产者停止后调用"。**当前采集切换路径没有调用它**：`CaptureManager` 的
switch 事务会 stop 旧流再 start 新流，跨越切换点时 packetizer 里可能残留半个旧设备的帧，切换后由新设备的 PCM 补齐并发出。
影响范围是一帧以内的拼接（≤ F 个 sample frame），值得知悉；若要求严格的时间线洁净，应在事务的空档期调用 `reset()`。

## 测试重点

`tests/audio/packetizer/` 覆盖：单 block、多 block 拼帧、一次 push 产生多帧、尾部 pending、未对齐输入、sequence 连续性，
以及 sink 不消费时 packetizer 自身行为保持确定。
