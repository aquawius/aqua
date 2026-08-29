# 模块：AudioFrameQueue

## 文件

- `aqua_core/include/aqua/audio/queue/audio_frame_queue.h`

## 职责

`AudioFrameQueue` 是 Server capture realtime thread 到 network worker 的固定容量 SPSC 队列。它的核心目的不是“提供一个普通队列”，而是**把 realtime callback 与可能发生系统调用/网络发送的 worker 隔离开**。

## 拓扑

```text
WASAPI Capture RT
      │
      │ push(frame copy)
      ▼
┌─────────────────────┐
│ AudioFrameQueue     │
│ preallocated slots  │
└─────────────────────┘
      │
      │ consume_one
      ▼
AudioNetworkDispatcher
      │
      ▼
UDP worker
```

## 固定容量

所有 slot 在构造时分配。运行期 `push()` 不扩容、不分配。队列满时当前策略是**丢弃 newest**：已经在队列里的较早 frame 保留，新的 frame 不进入网络路径。

## 内存可见性

生产者写 slot payload 后通过 atomic head release 发布；消费者 acquire 后才能读取。消费者回调返回后才 release tail，生产者此后才能复用该 slot。这样可以保证 borrowed frame 在 consumer 回调整个期间有效。

## `consume_one` 生命周期

`consume_one(consumer)` 不是把 `AudioFrame` 所有权交给 consumer。consumer 拿到的是 queue slot 的借用视图：

```text
pop/acquire
   │
   ▼
consumer(frame)
   │
   └── callback 返回
             │
             ▼
        release tail
```

因此 consumer **不能保存 span、不能异步使用 frame、不能在 callback 返回后继续访问 slot**。

## wake hint

`should_notify()` 是性能提示，不是正确性机制。Producer 在 publish 后观察 consumer tail，决定是否需要通知 worker。即使通知丢失，worker 后续仍可自行 drain；通知只能减少空转延迟，不能决定 frame 是否存在。

## Realtime 要求

`push()` 必须 bounded、no allocation、no blocking、no logging I/O。任何网络操作都必须留在 Dispatcher worker。

## 统计

队列至少提供 accepted / consumed / dropped 统计。`dropped` 表示队列满导致的 newest-drop，不应和 UDP queue drop、JitterBuffer late/busy drop 混为一谈。

## 为什么不用 mutex queue

这里的生产者和消费者拓扑固定为一对一，SPSC 队列可以把锁竞争从 capture RT path 中完全拿掉，并让内存所有权边界显式化。引入通用 MPMC 容器只会增加不必要的同步与生命周期复杂度。
