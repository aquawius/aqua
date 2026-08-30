# aqua_client

## 最简启动

Client 最少只需要 Server IP：

```text
./aqua_client_cli --server-ip 192.168.1.10
```

默认配置为：

```text
Server gRPC：<server-ip>:50051
UDP：从 gRPC Connect 响应获取
回放设备：系统默认 OUTPUT 设备
回放格式：使用 Server 返回的音频格式
JitterBuffer：30 slots
Client 名称：aqua-client
```

Client 不需要手动指定 UDP 端口；Server 会在 gRPC Connect 响应中提供 UDP endpoint。Server 默认使用 `0.0.0.0:50000` 监听，因此正常部署时 Client 只需要知道 Server 的可达 IP。

## 参数

```text
--server-ip            必填，Server 的可达 IPv4/IPv6 地址
--server-rpc           Server gRPC 端口，默认 50051
--force-udp-port       覆盖 Server 下发的 UDP 端口；省略=使用 Server 通告的端口
--name                 Client 名称，默认 aqua-client
--jitter-slots         JitterBuffer 容量，默认 30，范围 4..4096
--device-id            OUTPUT 回放设备 ID；省略=系统默认 OUTPUT 设备
--log-level             trace|debug|info|warn|error|fatal
--list-devices         列出 OUTPUT 设备后退出
--help                 显示帮助
```

## 设备语义

Client 的 `--device-id` 始终表示 OUTPUT 回放 endpoint。显式指定时，CLI 会尽早检查该 ID 是否能够解析为 OUTPUT 设备；不要传入 INPUT 设备 ID。

## 音频格式

Client 不在 CLI 中指定采样率、声道数和编码。gRPC Connect 返回的 Server AudioFormat 是整个会话的权威格式，Client 使用该格式创建 JitterBuffer 和回放流。

因此 Client 使用系统默认回放设备时，也必须保证该设备能够播放 Server 提供的格式。

## Server 地址与 UDP

Client 只负责提供 Server IP 和可选的 gRPC 端口。UDP 默认完全采用 Server 通过 gRPC 下发的地址端口；`--force-udp-port` 仅覆盖端口，不覆盖地址，主要用于 NAT/端口映射等场景。

连接成功后：

```text
Server gRPC
→ Connect
→ 获取 session_id / UDP endpoint / AudioFormat / F
→ 选择 UDP endpoint（默认 Server 通告端口；指定 `--force-udp-port` 时仅替换端口）
→ 接收 AudioFrame
```

当 Server 通告的是 `0.0.0.0` 或 `::` 时，Client 使用 `--server-ip` 作为 UDP 目标 IP。Client 不提供 `force IP` 覆盖项；Server 应通过 `--advertise-ip` 正确提供客户端实际可达的 UDP 地址。

## 退出

```text
停止回放
→ 停止 UDP
→ 最佳努力发送 Disconnect
→ Runtime 停止
→ 进程退出
```
