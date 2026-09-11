# 模块：gRPC Control Plane

## Client

`GrpcClient` 是同步 API，外加一条探活 ping 线程：

```text
connect_to_server(server_ip, rpc_port)
connect(client_name, result)
start_keepalive(session_id, interval, handler)  # 内部 ping 线程周期调用，
                                                # 传输连续失败达阈值或会话不在
                                                # 即调 handler 一次随后退出
disconnect(session_id)
stop_keepalive()                                # 置停止标志 + TryCancel + join（幂等，析构自动调）
```

`ConnectResult` 只描述控制面能确定的信息：`advertised_udp_address` / `advertised_udp_port`（gRPC 通告的 UDP 端点，
wildcard 时已在此 fallback 到 concrete server IP）。数据面实际对端 `learned_udp_*` 不属于 gRPC 控制面，由上层
（C API `aqua_connect_result_t` / `UdpClient::learned_peer_endpoint()`）动态采样。

它保存最后一次成功连接的 concrete server IP，用来处理 wildcard UDP advertised address fallback。

## Server

`GrpcServerService` 只处理：

- Connect -> SessionManager.create_session + response
- Disconnect -> SessionManager.remove_session
- Keepalive -> SessionManager.touch_session_liveness + session_valid

它不做 UDP keepalive，不碰 JitterBuffer，不发送音频。三个 handler 都是短平快的一元
RPC（无长连接、无注册表、无后台线程），不存在阻塞 handler 拖住 `server_->Shutdown()`
的问题。

## Service 生命周期

`GrpcServer` 构造期间 BuildAndStart；`run()` 在独立 worker thread Wait；`shutdown()` 只负责通知退出。service 生命周期必须长于
gRPC server，因此成员声明顺序有意设计为 server 先析构、service 后析构。

## 输入限制

Connect client_name 1..128 bytes（`GRPC_MAX_CLIENT_NAME_BYTES`），越界返回 `INVALID_ARGUMENT`。server 下发的 format / F 必须
有效；response 必须能让 client 直接构造 playback 与 JitterBuffer。

## RPC 面

`aqua.pb.AudioService` 有三个方法：

```text
Connect(ConnectRequest{client_name}) -> ConnectResponse{session_id, udp{address,port}, audio_format, frame_count}
Disconnect(DisconnectRequest{session_id}) -> Empty
Keepalive(KeepaliveRequest{session_id}) -> KeepaliveResponse{session_valid}
```

- Connect 超时 `GRPC_CONNECT_DEADLINE = 3000ms`；Disconnect `GRPC_DISCONNECT_DEADLINE = 1000ms`；
- Disconnect 幂等，session 不存在也返回 OK；
- 通道使用 `InsecureChannelCredentials`，明文无鉴权（见 `../protocol.md` §9）。

## 存活模型（应用层，不依赖 gRPC 内部调参）

传输层 channel 参数保持默认（刻意不限 ping、不调 keepalive——版本相关的
GOAWAY 调参是事故之源）。存活判定只看应用层结果：

```text
client ping 线程：每 GRPC_KEEPALIVE_INTERVAL (1s) 一次带 deadline (800ms) 的 Keepalive，传输连续失败 GRPC_KEEPALIVE_MISS_THRESHOLD (5) 次判死，SessionGone 立即
server handler：存在即刷新 last_seen 并返回 valid=true；不存在返回 valid=false
client 判定：传输失败或 valid=false → Degraded（supervision 停服），不重试
```

常量共 6 个，都在 `grpc_config.h`：`GRPC_CONNECT_DEADLINE`(3000) / `GRPC_MAX_CLIENT_NAME_BYTES`(128) / `GRPC_DISCONNECT_DEADLINE`(1000) / `GRPC_KEEPALIVE_INTERVAL`(1000) / `GRPC_KEEPALIVE_DEADLINE`(800) / `GRPC_KEEPALIVE_MISS_THRESHOLD`(5)。

## 地址通告

```text
server 侧 effective_advertised_udp_address
    = advertised_udp_address 为空 ? server_ip : advertised_udp_address
advertised_udp_port = 显式配置值 ?: 实际绑定的 udp_port
```

通告地址允许是 wildcard（`0.0.0.0` / `::`）：client 发现 `is_unspecified()` 时回退到 gRPC 连接所用的 `server_ip`，端口仍用响应
中的端口。若端口为 0 或超过 65535，client 判定整笔 Connect 作废并 best-effort Disconnect 回滚。

## Service 生命周期

`GrpcServer` 构造期间 BuildAndStart；`run()` 在独立 worker 线程 Wait；`shutdown()` 只通知退出。service 生命周期必须长于 gRPC
server，因此成员声明顺序有意设计为 server 先析构、service 后析构。
