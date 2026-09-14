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
| `rx_errors`               | 接收错误（已排除 `operation_aborted`，即主动 stop 不算错误）         |
| `tx_packets` / `tx_bytes` | 成功发出的 datagram 数与字节数                                       |
| `tx_errors`               | 发送错误（非预期关闭）                                               |
| `tx_dropped`              | 队列溢出或清队导致的丢弃                                             |
| `tx_enqueue_failures`     | 入队失败或 pump 调度失败                                             |
| `tx_queue_depth`          | 当前待发队列长度（**不含 in-flight 那一包**，实际待发量最多为它 +1） |
