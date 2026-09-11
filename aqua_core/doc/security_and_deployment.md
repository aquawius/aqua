# 安全与部署限制

## 1. 当前威胁模型

当前协议适合可信局域网/实验部署，不是认证后的互联网协议。三个明确的缺口：

1. UDP heartbeat 只携带 session_id：拿到 session_id 的主机可以伪造 heartbeat 覆盖 endpoint（劫持音频流）；
2. Audio datagram 不携带任何身份信息：服务端对音频来源**不做校验**，任何能到达服务端 UDP 端口的主机都可以注入音频源；
3. gRPC 使用 `InsecureChannelCredentials`：控制面明文且无鉴权。

client 侧唯一的来源约束是"Audio 的 sender 必须等于 `learned_peer_endpoint`"，而 `learned_peer_endpoint` 来自 HeartbeatAck 的 sender，
在威胁模型内等价于"信任首个应答者"。

## 1.1 局域网部署下的严重性定级（2026-09 修订）

本项目只部署在可信局域网，上述缺口按**接受风险**处理，不安排修复：

- 单包伪造 heartbeat `learned_peer_endpoint` / 伪造 heartbeat 覆盖 server 端 endpoint：需同一 LAN 内的主动攻击者；
  lambda 家庭/办公 LAN 内无此动机时不处理。公网暴露前必须先做 token/AEAD（§3 不变）。
- `Connect` 无界创建 / session 碰撞循环 / `random_device` 跨实例竞争：LAN 内 client 数为个位数，
  只修了无竞争的 RNG 与碰撞重试上限（防测试并行误伤），不做配额/限流/幂等键。
- 功能性修复优先：JitterBuffer 别名污染、U8 静音电平、C API/JNI 边界、诊断计数、切换语义——这些在可信 LAN 内
  也会天天触发，已在本轮修复。

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
