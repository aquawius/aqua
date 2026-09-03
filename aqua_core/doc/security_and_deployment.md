# 安全与部署限制

## 1. 当前威胁模型

当前协议适合可信局域网/实验部署，不是认证后的互联网协议。三个明确的缺口：

1. UDP HELLO 只携带 session_id：拿到 session_id 的主机可以伪造 HELLO 覆盖 endpoint（劫持音频流）；
2. Audio datagram 不携带任何身份信息：服务端对音频来源**不做校验**，任何能到达服务端 UDP 端口的主机都可以注入音频源；
3. gRPC 使用 `InsecureChannelCredentials`：控制面明文且无鉴权。

client 侧唯一的来源约束是"Audio 的 sender 必须等于 `learned_endpoint`"，而 `learned_endpoint` 来自 HELLO_ACK 的 sender，
在威胁模型内等价于"信任首个应答者"。

## 2. 绑定与通告

`server_ip` 表示 gRPC 与 UDP 共用的本地监听地址；`advertised_udp_address` / `advertised_udp_port` 表示发给 client 的 UDP
目标地址端口。监听 `0.0.0.0` 并不意味着 client 应该向 `0.0.0.0` 发送；wildcard advertised address 由 Client 回退到 gRPC
连接所用的 concrete server_ip。

如果 advertised address/port 未显式设置，ServerRuntime 分别继承 `server_ip` / `udp_port`；若通告地址最终是
wildcard，ConnectResponse 使用 wildcard sentinel，client fallback 到 gRPC 连接使用的 Server IP。

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

固定 UDP server 不启用 `SO_REUSEADDR`，采用单 owner 监听模型；内核 UDP buffer 显式增大到 64 KiB，应用层另有有界 datagram
queue。两者分别解决内核突发与应用异步发送积压。
