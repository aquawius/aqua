# 模块：UDP

## NetworkFrame

职责是纯 wire codec：

```text
PacketType + sequence/session_id + payload
```

不拥有 decode 后 payload。decode view 只在输入 datagram buffer 存活期间有效。

## UdpTransport

Transport 是底层 socket + strand + pending send queue。它统一处理：

- bind/open
- remote
- async receive
- queued async send
- socket buffer size
- stop
- transport counters

所有跨线程 async handler 持有 shared `State`，避免 transport 对象析构后 handler 反向访问悬空 this。

应用发送队列上限 64 datagrams；pending 满按 drop-oldest，in-flight datagram 单独持有，不会被淘汰。

## UdpServer

只接收 HELLO，其他 packet type 统计为 non-hello。广播前从 SessionManager snapshot Connected endpoints，然后把同一个 immutable encoded datagram 发往多个 endpoint。

## UdpClient

启动 receive 时指定 expected audio payload bytes；只有严格匹配的 Audio packet 才交给 callback。HELLO timer 每秒运行，并维护 ack miss 状态。

## UDP buffer 与应用 queue

二者不要混淆：

```text
kernel SO_RCVBUF/SNDBUF = 吸收 OS/network burst
app pending queue       = 控制 async send backlog
JitterBuffer            = playback timeline buffer
```
