# Aqua Protocol

## 1. Control plane

gRPC service：

```text
Connect(ConnectRequest) -> ConnectResponse
Disconnect(DisconnectRequest) -> Empty
```

ConnectRequest 当前包含：

```text
client_name
```

ConnectResponse：

```text
session_id
udp.address
udp.port
audio_format
frame_count
```

## 2. UDP endpoint semantics

Server 有三个不同地址概念，其中 bind 与 advertisement 必须分开理解：

```text
udp_bind_ip
    = 本机 socket bind 地址，可为 0.0.0.0 / ::

advertised_udp_address
    = 返回给 client 的 UDP address hint；未显式设置时由 Runtime 继承 udp_bind_ip，可以是 wildcard sentinel
```

为了兼容普通 server 配置，advertised address **允许**为：

```text
0.0.0.0
::
```

Server 未显式提供 `advertise-ip` 时，Runtime 从 `udp_bind_ip` 派生 advertised address；显式地址优先。wildcard `0.0.0.0` / `::` 是正式支持的 sentinel。Server 对显式 `advertise-ip` 做 IP 字面量校验；Client 为兼容旧版本或异常响应，也把空/无法解析的通告地址视为不可用，并执行 fallback。Client 不会把 wildcard/空/非法值直接用于 UDP，而是：

```text
ConnectResponse.udp.address is wildcard
        ↓
use the server IP used for the gRPC connection (`ClientRuntimeConfig.server_ip`)
        ↓
UDP remote endpoint
```

因此：

```text
server --udp-ip 0.0.0.0 --advertise-ip 0.0.0.0
client --server-ip 192.168.1.20
```

最终 client 使用：

```text
192.168.1.20:<udp-port>
```

显式、可解析且 non-wildcard 的 advertised address 优先使用，用于多网卡/特殊路由部署。

## 3. UDP wire format

### Audio

```text
byte 0      type
byte 1..8   sequence, little-endian uint64
rest        PCM payload
```

### HELLO / HELLO_ACK

```text
byte 0      type
byte 1..4   session_id, little-endian uint32
```

Audio payload 上限：1443 bytes。

## 4. Session / HELLO

流程：

```text
Client --Connect--> Server
Client <--session+UDP+format-- Server
Client --HELLO----------------> Server
Client <--HELLO_ACK------------ Server
Client --Audio receive--------> playback
```

Server 收到 HELLO 后：

1. 校验 session_id；
2. 建立或刷新 endpoint；
3. 更新 last_seen；
4. 回 HELLO_ACK。

Client 根据 HELLO_ACK generation 判断 liveness；连续 miss 达到阈值后进入 Degraded。

## 5. AudioFrame contract

一次 session 中：

```text
AudioFormat 固定
frame_count F 固定
AudioFrame payload = F × frame_bytes
```

sequence 只由 Server Packetizer 产生；UDP 不改序列号。

## 6. Trust model

当前协议为 trusted-LAN protocol：session_id 本身不是 authentication credential，HELLO 没有 token/HMAC。公网部署不在当前 security baseline 内；见 `security_and_deployment.md`。

## 7. ConnectRequest validation

`client_name` 必须为 1..128 bytes。非法值由 server 直接返回 `INVALID_ARGUMENT`，不创建 session。
