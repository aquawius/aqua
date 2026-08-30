# 模块：Runtime

## ClientRuntime 启动顺序

源码实现的启动顺序是：

```text
Created
  ↓
validate client config
  ↓
create DeviceManager
  ↓
create Playback backend
  ↓
gRPC connect_to_server
  ↓
gRPC Connect
  ↓
validate returned format/F/payload geometry
  ↓
create JitterBuffer + playback pipeline
  ↓
configure UDP remote
  ↓
start UDP receive
  ↓
start HELLO keepalive
  ↓
start AudioPlayback
  ↓
Running
```

这个顺序的核心目的是：在 playback callback 开始前，所有需要的网络目标、格式、F 和 JitterBuffer storage 已经准备完成。

### 失败语义

任意一个步骤失败都会回到 `stop_locked()`，已经创建的前序资源按逆向安全顺序清理。Runtime 不把“半启动”状态暴露给调用者。

## ClientRuntime 数据路径

UDP handler 创建：

```cpp
AudioFrame{sequence, frame_count, pcm_view}
```

然后调用 `jb->push()`。JitterBuffer 复制数据，不依赖 UDP receive buffer 生命周期。

Playback callback 则只做：

```text
pull(output) -> JitterBuffer::pull(output)
```

## ServerRuntime 启动顺序

设备与格式在 **构造期**（成员初始化）已完成，`start()` 只校验并复用：

```text
构造期：
  resolve capture device  -> effective_capture_device_
  resolve AudioFormat     -> effective_format_
  resolve F（auto/explicit，MTU 校验）-> effective_frame_count_
  创建 SessionManager / UdpServer / Packetizer / FrameQueue / Dispatcher
```

`start()` 实际顺序：

```text
Created -> Starting
validate format / F / queue slots / timeout / port / geometry
create ReapState（reaper strand + timer）
create capture backend
bind UDP + start UDP receive loop
start dispatcher worker
start capture（校验 backend 实际 format == effective_format_）
construct gRPC service/server
spawn gRPC worker
Running
schedule reaper on strand
```

设备与格式在构造期冻结、start 期只校验，保证「探测」与「启动」用的是同一个 endpoint。

## Server 数据路径

Capture callback：

```text
AudioBlock
  ↓ packetizer.push
0..n AudioFrames
  ↓ queue.push
publish_from_realtime()
```

dispatcher worker 被唤醒后 drain queue。每帧 encode 一次，再 broadcast 到当前 Connected sessions。

## Degraded

运行期 backend event（例如设备失效）或网络 liveness terminal condition 可以把 runtime 标记为 `Degraded`。CLI control poll
会观察到这一状态并执行 stop/exit；Runtime 本身不自行把 stop 深埋在 realtime/event callback 中。
