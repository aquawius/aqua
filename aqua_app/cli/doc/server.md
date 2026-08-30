# aqua_server

## 最简启动

无需参数即可启动：

```text
./aqua_server_cli
```

默认配置为：

```text
Server 监听地址：0.0.0.0
gRPC：50051
UDP：50000
捕获方式：loopback
捕获设备：系统默认 OUTPUT 设备
音频格式：由捕获后端提供的默认格式
frames-per-slot：自动按 UDP MTU 预算推导
网络队列：4 slots
```

Server 使用一个 `server-ip` 作为本地监听地址，gRPC 与 UDP 共用这个地址。

`advertise-ip` / `advertise-udp-port` 是单独的“通知客户端的 UDP 地址端口”，用于 NAT、多网卡、容器等场景。省略时分别继承 `server-ip` / `udp-port`。

当通告 IP 为 `0.0.0.0` 或 `::` 时，Client 会使用自己连接 gRPC 时指定的 `--server-ip` 作为实际 UDP 目标 IP。

## 参数

```text
--server-ip             gRPC/UDP 共用的本地监听 IP，默认 0.0.0.0
--rpc-port              gRPC 监听端口，默认 50051
--udp-port              UDP 数据面监听端口，默认 50000
--advertise-ip          通知客户端的 UDP IP，默认跟随 --server-ip
--advertise-udp-port    通知客户端的 UDP 端口，默认跟随 --udp-port
--encoding              s16|s24|s32|f32|u8；需与 channels/sample-rate 一起指定
--channels              声道数；需与 encoding/sample-rate 一起指定
--sample-rate           采样率 Hz；需与 encoding/channels 一起指定
--frames-per-slot       AudioFrame 帧数；0=自动，显式值 >=16 且必须满足 MTU 预算
--capture               捕获方式：loopback|input，默认 loopback
--device-id             捕获设备 ID；loopback 使用 OUTPUT，input 使用 INPUT
--session-timeout-ms    Session 超时，默认 5000
--reap-interval-ms      Session 清理周期，默认 1000
--network-queue-slots   捕获到网络的交接队列，默认 4，范围 1..4096
--log-level             trace|debug|info|warn|error|fatal
--list-devices          列出 INPUT/OUTPUT 设备后退出
--help                  显示帮助
```

## 捕获设备语义

Server 的捕获源只有两种：

- `--capture=input`：从 INPUT endpoint 捕获，例如麦克风。
- `--capture=loopback`：从 OUTPUT endpoint 做 WASAPI loopback 捕获，例如扬声器、耳机、数字输出的系统混音。

因此 OUTPUT 设备不能与 `--capture=input` 搭配，INPUT 设备也不能用于 `--capture=loopback`。显式 `--device-id` 会在 CLI 阶段尽早检查设备是否能够解析为所需方向；省略时使用该方向的系统默认设备。

例如捕获数字输出设备：

```text
--capture loopback --device-id "{0.0.0.00000000}.{...}"
```

## 格式

`--encoding`、`--channels`、`--sample-rate` 要么全部指定，要么全部省略。

全部省略时，由 capture backend 获取目标设备的默认共享模式格式。显式指定时，必须满足 `AudioFormat::is_valid()`，并且 `F × frame_bytes` 不得超过 UDP 安全 payload 预算。

## frames-per-slot

`0` 表示自动按当前音频格式和 UDP MTU 预算计算 F。

显式指定时要求：

```text
F >= 16
F × frame_bytes <= UDP_AUDIO_PAYLOAD_BYTES
```

## 启动流程

```text
解析 CLI
→ 创建 ServerRuntime
→ 解析捕获设备与默认格式
→ 绑定 UDP
→ 启动捕获与网络分发
→ 启动 gRPC
→ Running
```
