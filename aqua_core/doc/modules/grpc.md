# 模块：gRPC Control Plane

## Client

`GrpcClient` 是同步 API，外加一条探活 ping 线程：

```text
connect_to_server(rpc_ip, rpc_port)
connect(client_name, result)
start_keepalive(session_id, interval, handler)  # 内部 ping 线程周期调用，
                                                # 传输连续失败达阈值或会话不在
                                                # 即调 handler 一次随后退出
disconnect(session_id)
stop_keepalive()                                # 置停止标志 + TryCancel + 有条件 join（幂等，析构自动调）
```

keepalive 与其余自有线程统一是 `std::jthread` + `stop_token`（见 `threading_and_lifecycle.md` 第7节）：
等待期仍是 100ms 切片（stop 最多延迟一拍，沿用原语义），停止是 `request_stop()` +
`TryCancel()` 在途 RPC + join。join 前仍判 `get_id() != this_thread::get_id()`——ping 线程内调
`stop_keepalive()` 时只取消不 join（线程随后自然退出，由析构路径回收），jthread 在此并不比
`std::thread` 更安全，只是统一了线程类型，消掉一个原子停止标志。

`ConnectResult` 只描述控制面能确定的信息：`advertised_udp_address` / `advertised_udp_port`（**已按客户端族选中的槽**，
sentinel 时已在此替换为 concrete server IP）+ `udp_family`（选中的族，= 客户端自己的族）+
`udp_address_from_sentinel`（地址是否来自 sentinel 回落）。数据面实际对端 `learned_udp_*` 不属于 gRPC 控制面，由上层（C API
`aqua_connect_result_t` / `UdpClient::learned_peer_endpoint()`）动态采样。

它保存最后一次成功连接的 concrete server IP：既用于 sentinel 槽的回落，也是**判定客户端地址族**的依据（`udp` / `udp6` 选哪个槽）。

## Server

`GrpcServerService` 只处理：

- Connect -> SessionManager.create_session + response
- Disconnect -> SessionManager.remove_session
- Keepalive -> SessionManager.touch_session_liveness + session_valid

它不做 UDP keepalive，不碰 JitterBuffer，不发送音频。三个 handler 都是短平快的一元 RPC（无长连接、无注册表、无后台线程），不存在阻塞
handler 拖住 `server_->Shutdown()`
的问题。

## Service 生命周期

`GrpcServer` 构造期间 BuildAndStart；`run()` 在独立 worker thread Wait；`shutdown()` 只负责通知退出。service 生命周期必须长于
gRPC server，因此成员声明顺序有意设计为 server 先析构、service 后析构。

## 输入限制

Connect client_name 1..128 bytes（`GRPC_MAX_CLIENT_NAME_BYTES`），越界返回 `INVALID_ARGUMENT`。server 下发的 format / F 必须
有效；response 必须能让 client 直接构造 playback 与 JitterBuffer。

`client_name` 的长度不变量**由客户端 Core 提前守**（`runtime::validate_client_identity`，由
`ClientRuntime::start()` 调用），CLI parser 只转译 Core 返回的原因。这样做的原因：否则
C API / JNI 这两个不经过 CLI 的公共入口可以塞进一个必然被服务端拒的名字，而调用方只能
拿到一个无信息量的"控制面连接失败"——与 `validate_client_endpoint`（INV-8）同一条理由。
Android 侧另在输入层限长（`AquaController.CORE_MAX_CLIENT_NAME_BYTES`）并截断。

## RPC 面

`aqua.pb.AudioService` 有三个方法：

```text
Connect(ConnectRequest{client_name}) -> ConnectResponse{session_id, udp{address,port}, udp6{address,port}, audio_format, frame_count}
Disconnect(DisconnectRequest{session_id}) -> Empty
Keepalive(KeepaliveRequest{session_id}) -> KeepaliveResponse{session_valid}
```

- Connect 超时 `GRPC_CONNECT_DEADLINE = 3000ms`；Disconnect `GRPC_DISCONNECT_DEADLINE = 1000ms`；
- Disconnect 幂等，session 不存在也返回 OK；
- 通道使用 `InsecureChannelCredentials`，明文无鉴权（见 `../protocol.md` 第9节）。

## 存活模型（应用层，不依赖 gRPC 内部调参）

传输层 channel 参数保持默认（刻意不限 ping、不调 keepalive——版本相关的 GOAWAY 调参是事故之源）。存活判定只看应用层结果：

```text
client ping 线程：每 GRPC_KEEPALIVE_INTERVAL (1s) 一次带 deadline (800ms) 的 Keepalive，传输连续失败 GRPC_KEEPALIVE_MISS_THRESHOLD (5) 次判死，SessionGone 立即
server handler：存在即刷新 last_seen 并返回 valid=true；不存在返回 valid=false
client 判定：传输失败或 valid=false → Degraded（supervision 停服），不重试
```

常量共 6 个，都在 `grpc_config.h`：`GRPC_CONNECT_DEADLINE`(3000) / `GRPC_MAX_CLIENT_NAME_BYTES`(128) /
`GRPC_DISCONNECT_DEADLINE`(1000) / `GRPC_KEEPALIVE_INTERVAL`(1000) / `GRPC_KEEPALIVE_DEADLINE`(800) /
`GRPC_KEEPALIVE_MISS_THRESHOLD`(5)。

## 地址通告

```text
槽值（逐族） = --udp-advertise-ipv4/-ipv6 为空 ? 对应族的绑定地址 : 显式通告地址
              （绑定为通配 ⇒ 槽值就是通配字面量，即 sentinel）
advertised_udp_port = 显式配置值 ?: 实际绑定的 udp_port
槽是否存在          = 该族数据面绑定成功（槽不存在 = 不服务该族）
```

**客户端按自己的族选槽**：IPv4 客户端读 `udp`，IPv6 客户端读 `udp6`；槽字段不存在 = 本 server 不服务该族的
数据面，client 立刻判定整笔 Connect 失败并回滚（不静默回落到另一族）。客户端族在配置期就已唯一确定（`--rpc-ip`
必须是具体字面量），所以服务端不需要解析 `ctx->peer()` 来猜族——peer 族只用于 Connect 期的一道**防御性**准入
检查（`FAILED_PRECONDITION`；peer 族无法判定时不拒绝，宁可漏判，漏掉的由客户端侧的空槽硬失败兜住）。

槽值允许是 wildcard（`0.0.0.0` / `::`）：那是 **sentinel**，client 发现 `is_unspecified()` 时改用自己连控制面时所用的
`rpc_ip`（族必然与槽一致），端口仍用响应中的端口。若端口为 0 或超过 65535、或地址非法，client 判定整笔 Connect
作废并 best-effort Disconnect 回滚。
