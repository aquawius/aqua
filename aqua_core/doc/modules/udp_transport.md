# 模块：UdpTransport

## 1. 职责

`UdpTransport` 是 Aqua 唯一的通用 UDP socket/异步发送层。`UdpClient` 和 `UdpServer` 都建立在它上面。

它只知道：

- UDP endpoint；
- datagram bytes；
- Asio async receive/send；
- socket 生命周期；
- 有界发送队列；
- transport statistics。

它不知道：

- session 语义；
- Heartbeat；
- AudioFrame；
- PCM 格式；
- JitterBuffer。

## 2. State + strand

构造时只创建 `shared_ptr<State>`，State 内创建：

```text
strand
socket(strand)
handler
预分配 receive buffer
send queue
in-flight send
```

所有 socket 操作及 receive/send handler 都在该 strand 上执行。

对象本身的析构不直接要求排队 handler 先执行，因为 async handler 只捕获 `shared_ptr<State>`，不捕获 `this`。

因此：

```text
UdpTransport destructed
        │
        └─ State 仍可能由 handler 持有
```

不会形成 `this` use-after-free。

## 3. 配置阶段锁

`config_mutex_` 串行化：

- open
- bind
- set_remote
- stop
- local endpoint snapshot

这是控制面锁，不进入高频收发路径。

默认远端 endpoint 另有 `remote_mutex_`，因为业务线程可能调用 send，而控制线程可能设置 remote。

## 4. 打开模型

### Server

```cpp
bind(ip, port)
```

固定 listener。IPv6 socket 明确设置 `v6_only=true`；双栈需要两个 transport。

不启用 `SO_REUSEADDR`，采用单 owner 模型。

### Client

`open()` 使用本地临时端口；`set_remote(ip, port)` 若尚未 open，会根据 remote address family 建立合适 socket。

transport 一旦 stop 后不可复用；重连应创建新实例。

### 失败模型

`bind()` / `open()` / `set_remote()` / `start_receive()` 都返回 `std::expected<void, NetError>`（`[[nodiscard]]`），
失败原因进类型而不是一个 bool：

```text
Stopped                transport 已停止
AlreadyBound           已绑定，且请求的 endpoint 与当前不同（换地址族/端口需新建 transport）
InvalidEndpoint        IP 字面量非法，或端口为 0
AddressFamilyMismatch  远端地址族与已打开 socket 不一致
BindFailed             底层 open/bind 失败（端口占用、权限等，细节在日志）
NotOpen                socket 未打开（应先 bind/open 再 start_receive）
ReceiveStartFailed     接收循环启动失败（strand 投递 / 首次 async_receive 异常）
InvalidArgument        调用参数本身非法（payload 为 0 / handler 为空）
```

`UdpClient` 层在此之上再加一个 `AlreadyStarted`：数据面已启动后，`set_remote()` / `start_receive()` 的 配置或启动请求会被忽略并以此码返回。

错误码经 `net_error_name()` 渲染；`ServerRuntime` / `ClientRuntime` 的启动失败日志会带上该名字 （如
`failed to bind UDP 0.0.0.0:50000: bind_failed`）。枚举与名字表由 `static_assert` 对齐：加枚举值 不改表即编译失败。

### Windows：ICMP port unreachable 会毒化 socket

Windows 默认把上一次 `sendto()` 触发的 ICMP **port unreachable** 回送给本端 socket：
下一次 `async_receive_from` 立即以 `WSAECONNRESET`（回环上表现为 `WSAECONNREFUSED`）完成，
而且**之后每一次投递都会立即失败**，socket 从此再也收不到任何 datagram，直到进程重启。

触发条件只有一个：向一个已经没有监听者的 endpoint 发过包。典型场景是 client 被强杀
（没走 `Disconnect` RPC），server 还在按 5 秒 session 超时继续给它发音频。
因为 UDP server 是**一个 socket 服务所有 peer**，一个 peer 的死亡会瘫痪全部 peer：
表现为 session 照常超时删除，但新 client 的握手永远到不了（gRPC `Connect` 成功、
UDP 侧零收包）。

判据（日志三元组）：`rx` 冻结 + `rx_unreachable` 单调上涨 + 停止发送后仍在涨。

两层处理：

1. **源头关闭**：`open_and_bind_locked()` 里在 bind 之前调
   `WSAIoctl(SIO_UDP_CONNRESET/NETRESET, FALSE)`（KB 263823）。Linux/macOS 上未连接
   UDP socket 默认不报这类错误，所以这只是把 Windows 拉回其它平台的默认行为。
   同一做法见于 Go net、Rust tokio/mio、.NET runtime、pjsip、Dart/Flutter。
2. **兜底分类**：万一平台不支持 ioctl，`is_unreachable_noise()` 把这类错误归为
   `rx_unreachable` 噪声（**不计入 `rx_errors`**），接收循环按退避重武装、不终止。

`rx_consecutive_errors` 仍然对两类错误一起计数——它管的是“别让失败循环空转打满 CPU”，
与“这是不是真故障”是两件事。

## 5. Receive buffer

State 中有 64 KiB 用户态 receive array。每次 async_receive_from 复用同一 buffer。

因此 receive handler 参数：

```text
span<const byte> data
```

只在当前 handler 调用期间有效。上层如果需要跨 callback 保存，必须 copy。

64 KiB `SO_RCVBUF` 是 kernel queue 容量，不是 single datagram size。

## 6. Send queue

发送分为两级：

```text
application thread
       │
       ▼
user-space deque (max 64 datagrams)
       │
       ▼
one in-flight async_send_to
       │
       ▼
OS UDP socket
```

### copy send

`send_to(span)` 会复制到新的 shared vector，再入队。适合 Heartbeat/ACK 等低频小包。

### shared send

`send_to_shared(shared_ptr<const vector<byte>>)` 直接共享 immutable payload。Server 广播音频时只编码一次，然后多个
endpoint 共享同一份 vector。

### overflow

pending queue 满时 drop-oldest；in-flight 不在 pending queue 中，因此不会被淘汰。

如果 async_send_to 的发起过程异常，当前 in-flight 与剩余 pending 会明确被丢弃，并增加 enqueue failure / drop
counter，避免进入“永久半死队列”。

## 7. Send pump

连续调用 send 不会给每一个 datagram 都 post 一个 task。State 使用 `send_pump_scheduled` 把一批积压合并到一个 strand pump。

pump：

1. 从 deque 移动一个 PendingSend 到 in-flight；
2. async_send_to；
3. completion 清理 in-flight；
4. 若还有 pending 且未 stop，继续 pump；
5. 队列为空才清除 scheduled 标志。

## 8. Stop

stop 首先 atomic 设置 `stopped=true`，随后把 `close_state()` post 到 strand。

`close_state()`：

```text
receiving=false
clear send queue
clear receive handler
socket.cancel()
socket.close()
```

send/receive completion 收到 `operation_aborted` 且已经 stopped 时，不计作普通错误。

## 9. 统计

`UdpTransportStats` 的字段是多个 atomic 的独立快照，不承诺跨字段同一时刻一致。它只用于诊断，不用于事务判断。

| 字段                      | 含义                                                                 |
|---------------------------|----------------------------------------------------------------------|
| `rx_packets` / `rx_bytes` | 成功接收的 datagram 数与字节数                                       |
| `rx_errors`               | 接收错误（**真故障**；已排除 `operation_aborted`，主动 stop 不算错误） |
| `rx_unreachable`          | 对端不可达噪声：内核回送的 ICMP port/net unreachable。socket 仍可用，不计入 `rx_errors` |
| `tx_packets` / `tx_bytes` | 成功发出的 datagram 数与字节数                                       |
| `tx_errors`               | 发送错误（非预期关闭）                                               |
| `tx_dropped`              | 队列溢出或清队导致的丢弃                                             |
| `tx_enqueue_failures`     | 入队失败或 pump 调度失败                                             |
| `tx_queue_depth`          | 当前待发队列长度（**不含 in-flight 那一包**，实际待发量最多为它 +1） |
