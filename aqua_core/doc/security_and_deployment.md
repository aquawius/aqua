# Security & Deployment

## 1. 当前 threat model

Aqua 当前假设：

- gRPC/UDP 所在网络基本可信；
- client/server 是已知参与者；
- session_id 不是认证 token；
- 控制面默认使用 insecure gRPC credentials。

因此当前版本适合可信内网，不是公网安全协议。

## 2. Address deployment model

Server 可以合法绑定：

```text
0.0.0.0
::
```

这表示监听本机所有对应地址。

Server 返回给 Client 的 UDP address 可以同样为 wildcard；Client 必须把它解释为“未指定具体目的地址”，并回退到 gRPC 使用的 `server_ip`。

如果 server 有多网卡且 client 应走另一条路由，可显式指定：

```text
--advertise-ip <reachable-address>
```

## 3. Session risks

当前 session ID 由 instance + counter 构造，存在有限空间与长期回绕问题；HELLO 只依赖 session_id，没有独立认证。

因此公网版本必须增加至少：

```text
ConnectResponse -> unpredictable capability/token
HELLO -> token validation
```

更高要求的公网版本应使用 TLS + authenticated session establishment。

## 4. UDP exposure

UDP 端口必须只开放到真正需要的网络范围。不要把 wildcard bind 误认为“对外公布 wildcard 地址”；bind 是监听策略，advertise 是路由/发现策略。

## 5. Input validation

server/client CLI 与 core runtime 都执行资源上限与基本参数校验，防止：

- 超大 queue allocation；
- 非法 frame geometry；
- wildcard client target；
- 无效 audio format；
- 空 client name。


## 6. Address semantics

`0.0.0.0` / `::` 只表示本地 listener 的 wildcard bind；它不是远端可连接目标。Aqua 允许 server 把 wildcard 作为 advertised sentinel，由 client 回退到其实际使用的 gRPC server IP。

## 7. Input limits

Connect `client_name` 限制为 1..128 bytes；resource-bearing CLI/Core 配置也有硬上限，避免异常输入直接造成巨大预分配。
