# 安全与部署限制

## 1. 当前威胁模型

当前协议适合可信局域网/实验部署，不是认证后的互联网协议。主要原因是 UDP HELLO 只携带 session_id：拿到 session_id 的主机可以改变 endpoint。

## 2. 绑定与通告

`rpc_bind_ip` / `udp_bind_ip` 表示监听位置；`advertised_udp_address` 表示发给 client 的目标地址。监听 `0.0.0.0` 并不意味着 client 应该向 `0.0.0.0` 发送。

如果 advertised address 为空，ServerRuntime 从 UDP bind address 派生；若最终是 wildcard，ConnectResponse 使用 wildcard sentinel，client fallback 到 gRPC server IP。

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
