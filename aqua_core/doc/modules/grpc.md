# 模块：gRPC Control Plane

## Client

`GrpcClient` 是同步 API：

```text
connect_to_server(server_ip, rpc_port)
connect(client_name, result)
disconnect(session_id)
```

`ConnectResult` 只描述控制面能确定的信息：`advertised_udp_address` / `advertised_udp_port`（gRPC 通告的 UDP 端点，
wildcard 时已在此 fallback 到 concrete server IP）。数据面实际对端 `learned_udp_*` 不属于 gRPC 控制面，由上层
（C API `aqua_connect_result_t` / `UdpClient::learned_peer_endpoint()`）动态采样。

它保存最后一次成功连接的 concrete server IP，用来处理 wildcard UDP advertised address fallback。

## Server

`GrpcServerService` 只处理：

- Connect -> SessionManager.create_session + response
- Disconnect -> SessionManager.remove_session

它不做 UDP keepalive，不碰 JitterBuffer，不发送音频。

## Service 生命周期

`GrpcServer` 构造期间 BuildAndStart；`run()` 在独立 worker thread Wait；`shutdown()` 只负责通知退出。service 生命周期必须长于
gRPC server，因此成员声明顺序有意设计为 service 先析构、server 后析构。

## 输入限制

Connect client_name 1..128 bytes（`GRPC_MAX_CLIENT_NAME_BYTES`），越界返回 `INVALID_ARGUMENT`。server 下发的 format / F 必须
有效；response 必须能让 client 直接构造 playback 与 JitterBuffer。

## RPC 面

`aqua.pb.AudioService` 只有两个方法：

```text
Connect(ConnectRequest{client_name}) -> ConnectResponse{session_id, udp{address,port}, audio_format, frame_count}
Disconnect(DisconnectRequest{session_id}) -> Empty
```

- Connect 超时 `GRPC_CONNECT_DEADLINE = 3000ms`；Disconnect `GRPC_DISCONNECT_DEADLINE = 1000ms`；
- Disconnect 幂等，session 不存在也返回 OK；
- 通道使用 `InsecureChannelCredentials`，明文无鉴权（见 `../protocol.md` §8）。

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
server，因此成员声明顺序有意设计为 service 先析构、server 后析构。
