# aqua_client

## 参数

```text
--server-ip            必填，必须是 concrete IP literal
--server-rpc           必填，>0
--name                 默认 aqua-client
--jitter-slots         默认 30，范围 4..4096
--device-id            OUTPUT playback device id；省略=system default
--log-level            trace|debug|info|warn|error|fatal
--list-devices
--help
```

CLI 当前没有旧 Android 项目中的“jitter milliseconds / detect window / playback buffer size”参数。

## 启动流程

```text
parse
→ create io_context
→ ClientRuntime
→ gRPC Connect
→ validate server format/F/payload
→ create JitterBuffer
→ configure UDP
→ start UDP receive
→ start HELLO
→ start playback
→ Running
```

## Server format 是权威

Client 不指定 sample rate/channels/encoding。它接收 Server format 并尝试原生播放；backend 不支持则 start 失败。

## 地址

`--server-ip` 只接受 IPv4/IPv6 literal，不接受 hostname，也不允许 `0.0.0.0` / `::`。

## 退出

signal 或控制流程结束后：

```text
playback stop
→ UDP stop
→ Disconnect best effort
→ runtime stopped
→ process exit
```
