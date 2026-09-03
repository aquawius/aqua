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

## D9：格式在构造期一次解析并全程冻结；设备可运行期切换（2026-09 修订）

**格式**在 `ServerRuntime` 构造时确定（`effective_format_` / `effective_frame_count_`），`start()` 只校验并复用，capture 启动后
再核对 backend 实际 format。原因：packetizer / queue / MTU 几何必须在启动前固定，避免"探测用设备 A 的格式、启动却是设备 B"的
静默漂移。格式仍不可运行期切换。

**设备**已可在运行期切换（原决策为"必须 stop→restart"）：设备故障或默认设备变化由 `CaptureManager`（server）/
`PlaybackManager`（client）按候选链重建端点，会话与格式不受影响。修订原因：设备故障误杀整个会话不可接受；切换事务以
`Format immutable` 为前提，因此并不削弱 D9 的几何保证。构造期解析出的 `effective_capture_device_` 只用于探测格式，不再钉给
运行期流。

## D10：loopback 静默时用欠账驱动的时间轴补偿

WASAPI loopback 在最后一个 render client 退出后可能 quiescence、audio event 停发；切歌等场景还会出现"空事件"（signal 但不
产包）与部分饥饿。capture 不使用"超时且零包"这种窄触发条件，而是每轮唤醒（事件或 20ms 超时）按墙钟欠账与本轮真实交付对账，
欠多少立即补多少静音，盈余留存抵扣。真实数据恢复后直接续接，不追历史。契约归属明确：采集端保证时间轴以 1x 推进，client 的
JitterBuffer 只负责网络抖动，不替采集端的停滞擦屁股。

## D12：设备切换的四条不变式

```text
Session alive        restart 不触碰 gRPC / UDP / session
Format immutable     restart 后流格式必须与会话格式一致，无转码、无重协商
Endpoint replaceable capture / playback 流生命周期独立于会话生命周期
Timeline continuous  切换允许 packet gap，禁止 seq 重置、时间轴重置、会话重建
```

两侧对称式：client 切换由本侧 JitterBuffer 吸收间隙，server 切换由对岸 client 的 JitterBuffer 饥饿路径吸收——Server 不为切换
新增缓冲机制。决议细节见 `capture_switching_design.md` 与 `playback_switching_design.md`。

## D13：只用设备事件判断设备故障

切换的触发源白名单是 `DeviceDisconnected` 与设备集合/默认设备变化。禁止用静音、低能量、"长时间无音频"推断设备失效——
loopback 在没有 render client 时静默并产出合成静音是合法稳态，"活着但无声"不等于"设备坏了"。

## D11：wake 通知是提示，不是正确性机制

`AudioFrameQueue::push` 的 `should_notify` 与 dispatcher 的 `wake_generation_` 构成 generation+notify 唤醒协议：generation
每次 push 都推进，notify 仅在 consumer 可能休眠时发出。丢失 notify 不影响正确性（worker 每次醒来都重读 head/tail），notify
只减少空转延迟。
