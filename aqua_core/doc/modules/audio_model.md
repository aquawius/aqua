# 模块：Audio Model

## 文件

- `aqua_core/include/aqua/audio/audio_format.h`
- `aqua_core/include/aqua/audio/audio_block.h`
- `aqua_core/include/aqua/audio/audio_frame.h`
- `aqua_core/include/aqua/audio/audio_error.h`

## 设计职责

这一层只定义**音频字节的含义**，不关心 UDP、JitterBuffer、设备或线程。

## AudioFormat

`frame_bytes()` 是所有上下游几何计算的唯一来源：

```text
frame_bytes = channels × bytes_per_sample
slot_bytes  = frame_count × frame_bytes
```

所有乘法都做 overflow check。任何上层创建 buffer、验证 UDP payload、计算 packetizer pending size，都应从这个结果推导，禁止复制一套“位深→字节”的手算逻辑。

## AudioFrame

`AudioFrame::data` 是借用视图，不拥有数据。它在：

- Packetizer sink callback 期间指向 packetizer pending buffer；或
- UDP decode 后指向 receive buffer；或
- JitterBuffer push 期间作为输入 view。

因此接收方若要跨 callback 保存必须复制。JitterBuffer 会把 payload copy 到自己的预分配 slot。

## AudioBlock

AudioBlock 只是 backend 一次 callback 的 PCM view。它允许 backend callback 粒度与网络 slot 粒度不同；Packetizer 负责重切。
