# 模块：gRPC Control Plane

## Client

`GrpcClient` 是同步 API：

```text
connect_to_server(server_ip, rpc_port)
connect(client_name, result)
disconnect(session_id)
```

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

Connect client_name 1..128 bytes。server format/F 必须有效；response 必须能让 client 直接构造 playback/JitterBuffer。
