# 已冻结设计决策

## D1：Client playback 只有一个应用层 buffer

当前只有 JitterBuffer。删除独立 playback RingBuffer，避免双水位和双控制器。

## D2：容量基本单位是 slot/byte，不是 ms

`capacity_slots` 是配置实体；byte 用于内存预算。延迟只能由 `F / sample_rate` 与 N 的组合推导，不把“30ms”作为内部 buffer API
的原始单位。

## D3：Server 的 audio handoff queue 与 playback buffer 分离

Server queue 只解决 capture RT -> network worker 的线程交接，不能被解释成播放 latency buffer。

## D4：丢帧决定在消费侧

Producer 只按 sequence/slot ownership 做接收过滤；真正的“现在应该跳过哪一帧”由 playback consumer 根据 play timeline
决定。这样网络抖动不会直接驱动 RT 时间轴倒退。

## D5：不实现 WASAPI Exclusive / Android Exclusive

产品目标是稳定的系统音频播放，不依赖独占模式。Windows 与 Android 都采用各自平台的 shared/non-exclusive 低延迟路径。

## D6：不做隐式格式转换

Server 格式是 session 契约。Client backend 不支持就拒绝启动 playback，而不是偷偷转换。

## D7：Runtime 管理生命周期，backend 管理 OS API

平台代码不得把 session、JitterBuffer 或 CLI policy 塞回 backend。

## D8：当前协议不安全于公网

session_id-only HELLO 是明确的 MVP 信任模型；后续如需公网必须引入认证 token/AEAD 等设计，而不能在现有协议上“默认认为安全”。

## D9：设备与格式在构造期一次解析，运行期不切换

Server 的 capture 设备与格式在 `ServerRuntime` 构造时冻结（`effective_capture_device_` / `effective_format_` /
`effective_frame_count_`），`start()` 只校验并复用，capture 启动后再核对 backend 实际 format。系统默认设备运行期变化不会静默切换
stream；换设备必须 stop→restart。原因：避免「探测用默认设备 A，启动时默认设备已变 B」的静默漂移。

## D10：loopback 静默时用合成静音保时间轴

WASAPI loopback 在最后一个 render client 退出后可能 quiescence、audio event 停发。capture 不用无限等待，而是 20ms
有界等待 + 主动探测；无数据时按墙钟合成静音 AudioBlock，让 capture 时间轴以 1x 速率继续推进。真实数据恢复后直接续接，不追历史、不回写时间轴。这保证
server 在系统静音期间仍持续向 client 出帧。

## D11：wake 通知是提示，不是正确性机制

`AudioFrameQueue::push` 的 `should_notify` 与 dispatcher 的 `wake_generation_` 构成 generation+notify 唤醒协议：generation
每次 push 都推进，notify 仅在 consumer 可能休眠时发出。丢失 notify 不影响正确性（worker 每次醒来都重读 head/tail），notify
只减少空转延迟。
