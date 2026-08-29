# Devices & Audio Format Design

## 1. 设备模型

`AudioDeviceId` 是 backend-specific 的不透明字符串；`AudioDevice` 只描述身份、名称、方向及 default 标记。

设备不携带固定格式。格式属于具体 stream。

## 2. 设备方向

```text
INPUT  -> microphone / capture endpoint
OUTPUT -> render endpoint
```

Loopback 不是第三类设备。它是：

```text
capture.source = OUTPUT_LOOPBACK
capture direction = OUTPUT
```

因此：

```text
server --capture=input --device-id <input-id>
server --capture=loopback --device-id <output-id>
```

没有 `--device-id` 时使用相应方向的 system default endpoint。ServerRuntime 的默认 source 为 OUTPUT_LOOPBACK，因此无参数 server 默认捕获系统默认 OUTPUT endpoint。Server 只指定 `--device-id` 时默认按 OUTPUT loopback 解释；要选择 INPUT endpoint，显式使用 `--capture=input --device-id <ID>`。Client 默认把播放送到系统默认 OUTPUT endpoint。

## 3. 查询界面

CLI 提供：

```text
server --list-devices
client --list-devices
```

Server 显示 INPUT 与 OUTPUT；Client 显示 OUTPUT。输出包含：

- `*`：系统默认设备；
- friendly name；
- backend-specific device id。

该设计保持脚本友好：列表用于发现，`--device-id` 用于稳定选择；不要求交互式菜单。Device ID 是 backend-specific opaque identity，换设备/换系统后可能变化，因此脚本不应假设 ID 跨机器永久稳定。

## 4. Server 格式决策

格式有两个来源：

### 显式配置

`--encoding + --channels + --sample-rate` 三项必须同时出现。后端必须原生支持，否则 start 返回 `FormatUnsupported`。

### Backend default

三项全部省略时：

```text
ServerRuntime
    ↓
AudioDeviceManager::default_format()
    ↓
selected capture endpoint
    ↓
WASAPI IAudioClient::GetMixFormat()
    ↓
AudioFormat
```

该格式在 Runtime 构造 packetizer / queue / dispatcher 前确定；capture start 之后再次检查 `AudioCaptureInfo::format`，若 backend 最终格式与预查询不一致则拒绝启动，避免网络侧按错误 geometry 工作。换言之，Server 的“零参数启动”本质上是动态设备发现 + backend format probe，而不是依赖静态的 F32/48kHz 假设。

## 5. frame_count

`frame_count == 0` 表示自动模式：

```text
F = floor(UDP_AUDIO_PAYLOAD_BYTES / frame_bytes)
```

显式 F 必须：

```text
F >= 16
F × frame_bytes <= 1443 bytes
```

这样每个 AudioFrame 在 IPv4/IPv6 下都不会形成 IP fragmentation。

## 6. Client playback format

Client 不自行决定网络流格式。ConnectResponse 返回的 AudioFormat 是当前 server stream 的权威格式，Client playback 使用该格式启动。Client 只能选择“输出到哪个 endpoint”，不能把同一 session 的网络数据悄悄当成另一个格式解释。

## 7. 当前 backend 语义

Windows/WASAPI：

- input：`eCapture` endpoint；
- loopback：`eRender` endpoint + loopback stream；
- playback：`eRender` endpoint。

未来 backend 必须保留以上 domain 语义，但平台能力可以不同。例如某平台不提供系统 loopback 时应返回 `NotSupported`，而不是伪造设备。
