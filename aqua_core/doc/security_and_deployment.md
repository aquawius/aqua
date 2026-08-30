# 安全与部署限制

## 1. 当前威胁模型

当前协议适合可信局域网/实验部署，不是认证后的互联网协议。主要原因是 UDP HELLO 只携带 session_id：拿到 session_id 的主机可以改变 endpoint。

## 2. 绑定与通告

`server_ip` 表示 gRPC 与 UDP 共用的本地监听地址；`advertised_udp_address` / `advertised_udp_port` 表示发给 client 的 UDP 目标地址端口。监听 `0.0.0.0` 并不意味着 client 应该向 `0.0.0.0` 发送；wildcard advertised address 由 Client 回退到 gRPC 连接所用的 concrete server_ip。

如果 advertised address/port 未显式设置，ServerRuntime 分别继承 `server_ip` / `udp_port`；若通告地址最终是 wildcard，ConnectResponse 使用 wildcard sentinel，client fallback 到 gRPC 连接使用的 Server IP。

## 3. 不做的安全功能

当前没有：

- session token
- datagram authentication
- encryption
- replay protection
- rate limiting
- peer identity verification

不要在公网直接暴露 UDP/gRPC 并把 session_id 当作认证凭证。

## 4. 运维边界

固定 UDP server 不启用 `SO_REUSEADDR`，采用单 owner 监听模型；内核 UDP buffer 显式增大到 64 KiB，应用层另有有界 datagram queue。两者分别解决内核突发与应用异步发送积压。
