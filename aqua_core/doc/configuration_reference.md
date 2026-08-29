# Aqua Configuration Reference

本文档定义当前 Server / Client CLI 与 RuntimeConfig 的默认行为，是配置语义的权威入口。

## 1. Server：零参数启动

Server 的设计目标是：**不提供任何参数即可启动**。

```text
aqua_server
```

等价于以下核心默认值：

| 配置 | 默认值 | 语义 |
|---|---|---|
| gRPC bind | `0.0.0.0:50051` | 监听所有 IPv4 本地接口 |
| UDP bind | `0.0.0.0:9999` | 监听所有 IPv4 本地接口 |
| advertised UDP address | inherit `--udp-ip` | 未显式指定时继承 UDP bind IP；UDP 端口始终使用实际 local endpoint port；wildcard IP 由 Client 用 `--server-ip` fallback |
| capture source | `loopback` | 捕获系统 OUTPUT endpoint 的 loopback stream |
| capture device | system default OUTPUT | 当前系统默认 render endpoint |
| audio format | backend default | 所选 capture endpoint 的默认/shared-mode 格式；Server 不主动调整 |
| frames-per-slot | auto | 根据实际 `frame_bytes` 和 UDP payload budget 自动计算 |
| network queue | 4 slots | capture → network worker 的有界交接队列 |

Server 只有在用户明确提供格式三元组时才改变默认格式：

```text
--encoding <s16|s24|s32|f32|u8> --channels <N> --sample-rate <Hz>
```

三项必须同时提供。没有任何格式参数时，**不能把默认值理解成 F32/48000 等固定值**；实际值由所选 capture endpoint 的 audio backend 决定。


### 最小启动路径

产品默认配置必须满足“先启动、后定制”的原则：

```text
Server: aqua_server
        ↓
        设备 = system default OUTPUT
        模式 = loopback
        格式 = 该设备/backend default
        F   = 按实际 frame_bytes 自动计算

Client: aqua_client --server-ip <IP> --server-rpc <PORT>
        ↓
        播放设备 = system default OUTPUT
        格式 = Server ConnectResponse 返回值
```

Server 可以进一步只增加一个 `--device-id <OUTPUT_ID>` 即切换 loopback 的输出端点；此时无需额外指定格式或其它运行参数。若目标是 INPUT endpoint，则使用 `--capture=input --device-id <INPUT_ID>`。

这一默认策略不是“静态猜一个格式”：Server 启动前先解析实际 capture endpoint，并向 backend 查询该 endpoint 的默认/shared-mode 格式；只有拿到该结果后才能确定 `frame_bytes`、自动 `frame_count`、packetizer 和网络 payload geometry。

## 2. Server CLI

| 参数 | 默认值 | 说明 |
|---|---|---|
| `--rpc-ip` | `0.0.0.0` | gRPC bind address；允许 wildcard |
| `--rpc-port` | `50051` | gRPC bind port |
| `--udp-ip` | `0.0.0.0` | UDP bind address；允许 wildcard |
| `--udp-port` | `9999` | UDP bind port；`0` 也可用于测试时请求系统分配端口 |
| `--advertise-ip` | same as `--udp-ip` | 未显式指定时继承 UDP bind IP；`0.0.0.0` / `::` 表示让 Client fallback 到 gRPC server IP |
| `--capture` | `loopback` | `loopback` 或 `input` |
| `--device-id` | system default | loopback → OUTPUT endpoint；input → INPUT endpoint |
| `--encoding/--channels/--sample-rate` | backend default | 三项必须同时指定；省略则采用 selected capture endpoint 的 backend default |
| `--frames-per-slot` | `0` | 自动从 UDP payload budget 推导；显式值必须 `>=16` 且不超过 payload budget |
| `--network-queue-slots` | `4` | `1..4096` |
| `--session-timeout-ms` | `5000` | > 0 |
| `--reap-interval-ms` | `1000` | > 0 |
| `--list-devices` | off | 查询 INPUT/OUTPUT endpoint 及其 backend default format 后退出 |

### Server 设备选择

```text
server --list-devices
server --capture=loopback --device-id <OUTPUT_ID>
server --capture=input --device-id <INPUT_ID>
```

Loopback 不是独立设备类型，而是对 OUTPUT endpoint 使用 loopback capture mode。

## 3. Client：只需两个连接参数

Client 的正常启动只要求两个 CLI 参数：

```text
aqua_client --server-ip <SERVER_IP> --server-rpc <SERVER_RPC_PORT>
```

这两个参数是唯一必填项。其它参数均有本地默认值。

| 参数 | 默认值 | 说明 |
|---|---|---|
| `--server-ip` | **必填** | gRPC server 的具体可达 IP；不能是 wildcard |
| `--server-rpc` | **必填** | gRPC server port |
| `--name` | `aqua-client` | `1..128` bytes |
| `--jitter-slots` | `30` | `4..4096` |
| `--device-id` | system default | playback OUTPUT endpoint |
| `--log-level` | `info` | 客户端日志级别 |
| `--list-devices` | off | 查询 OUTPUT endpoint 及 backend default format 后退出 |

例如：

```text
aqua_client --server-ip 192.168.1.20 --server-rpc 50051
```

## 4. Server → Client endpoint 规则

Server 的 `rpc-ip` / `udp-ip` 是本地 bind address，与 Client 要连接的远端地址不是同一个概念。

Server 可以合法地 bind：

```text
0.0.0.0
::
```

Server 默认也会把 `0.0.0.0` 作为 UDP advertised address 的 sentinel。ConnectResponse 中：

```text
advertised UDP address = 0.0.0.0 / :: / empty / invalid
        ↓
Client fallback
        ↓
--server-ip 对应的 concrete IP
```

UDP port 使用 Server 实际监听的 UDP local endpoint port，因此即使测试时 `--udp-port 0` 请求 OS 分配临时端口，Client 仍可获得正确端口。

显式提供的合法 non-wildcard advertised IP 优先于 fallback。

## 5. Format ownership

Server 是整个 session 的唯一 audio format authority。

决策规则：

```text
用户显式指定 encoding + channels + sample-rate
            │
            ├── backend 原生支持 → 使用该 format
            └── backend 不支持 → startup 失败

用户没有指定格式
            │
            ↓
selected capture endpoint 的 backend default format
```

**原则：除非用户特意指定格式，Server 不调整格式。** Server 不做自动重采样、自动声道转换或偷偷替换用户请求。Client 只能使用 ConnectResponse 返回的格式播放。

## 6. Configuration validation

CLI 负责用户体验层校验；Runtime 负责 Core invariant 校验。两层必须保持一致。

其中：

- `server-ip` 必须是 concrete IP，不能是 `0.0.0.0` / `::`；
- Server bind address 可以是 wildcard；
- advertised address 可以是 wildcard sentinel；
- `jitter-slots` 为 `4..4096`；
- `network-queue-slots` 为 `1..4096`；
- 显式 `frames-per-slot >= 16`，且 payload 不超过 1443 bytes；
- `client_name` 为 `1..128` bytes。
