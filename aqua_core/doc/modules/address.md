# 模块：Address

`parse_ip_address()` 只接受 IP literal，不解析 DNS hostname；可接受带 `[]` 的 IPv6 文本并先去括号。

`format_host_port()` 输出稳定格式：

```text
IPv4: 1.2.3.4:9999
IPv6: [2001:db8::1]:9999
```

这样日志和 advertised endpoint 不会把 IPv6 的冒号与 port 冒号混在一起。

非 IP 输入（主机名）不抛异常，`format_host_port()` 退化为原始 `host:port`（供日志等容错场景）；功能性调用方应先经
`parse_ip_address()` 校验。

## 地址族判定（`ip_family.h`）

族判定的唯一入口。两个函数**不能混用**，这是本模块最容易踩的地方：

```text
effective_family(addr)      归一化后的族：::ffff:a.b.c.d → V4（它服务的是 IPv4 客户端）
                            用途：会话/诊断的族标签、客户端族判定、gRPC peer 文本
is_native_family(addr, f)   文本形态是否该族的**原生**形态（v4-mapped 两种槽都不算）
                            用途：**任何会把地址原样交给套接字层的地方** ——
                              服务端：数据面绑定（--udp-ipv4/-ipv6）、通告地址（--udp-advertise-*）、
                                      控制面绑定（--rpc-ip）
                              客户端：拨号目标（--rpc-ip）、选中的通告槽地址、单族 bind 适配器
                            —— 用错（拿 effective_family 当它用）会让 ::ffff: 字面量
                            通过校验、随后开出 AF_INET6 socket（"文本是 v6、语义是 v4"）。
```

`FamilySet{v4, v6}` 表达"一个实例服务了哪些族"，`to_string()` 给日志用的稳定短名
（`v4+v6` / `v4` / `v6` / `none`）；`FamilySet::contains()` 对 `IpFamily::Unknown` 恒为假
（Unknown 只表示"尚未观测到族"，永不参与路由）。

`normalize_for_display()` 把 `::ffff:1.2.3.4` 还原成 `1.2.3.4`——**只用于日志/诊断/UI**。
绝不能用它的返回值替换"用于发送的存储 endpoint"：在 AF_INET6 socket 上用 AF_INET 目标 `send_to`
会直接 `EAFNOSUPPORT`。

`parse_grpc_peer_address()` 解析 gRPC `ServerContext::peer()` 的文本形态（`ipv4:a.b.c.d:p` /
`ipv6:[..]:p`，含 percent-encoded zone 的 best-effort 兼容，`::ffff:` 归一化为 V4）。
判不出族时返回 `nullopt`，调用方**必须**把它当作"未知"而不是"不匹配"——Connect 期的族准入检查
宁可漏判（由客户端侧的空槽硬失败兜住），也不能因为解析失败误拒合法客户端。

**实测形态（务必按这个写测试）**：gRPC 输出的是 URI 编码形态，方括号本身也被编码：

```text
ipv4:127.0.0.1:58307          ← 字面形态
ipv6:%5B::1%5D:58382          ← 实测！%5B=[ %5D=]（不是字面的 "[::1]"）
ipv6:%5B::ffff:1.2.3.4%5D:p   ← 双栈监听下的 v4 客户端
ipv6:%5Bfe80::1%2524%5D:p     ← zone 的 '%' 编码成 %25
ipv6:%5B::1%5D%3A58305        ← 防御性：连端口冒号也被编码
```

**实现是"剥掉 `ipv4:`/`ipv6:` 前缀后整段 percent-decode 一次，再做结构解析"**，
所以上面全部形态都被覆盖，而且**只解码一次**（`%25` → `%` 是 zone 的正确编码；
二次解码会把它变成垃圾）。peer 里出现**字面**（未编码）的 `%` 属 RFC 3986 违规形态，
本函数不承诺支持——此时返回 `nullopt`，调用方按"未知"处理（fail-open 不误拒）。

解析前统一做 percent-decode。**这条踩过坑**：只按字面 `[..]` 形态解析时，
`peer_family` 恒为 `unknown`，于是服务端的族准入检查静默失效（不拒绝），
退化成"客户端连上了却永远收不到 ACK"——正是这次改造要消灭的那个症状。
"宁可漏判"的规则本身没错，但它要求**解析器覆盖真实形态**，否则"漏判"会变成"永远漏判"。

### zone / scope id

带 zone 的 IPv6 文本（`fe80::1%24`）是**逐主机本地**的接口索引，跨主机传递没有意义，因此：

```text
wire 上永不携带 zone：--udp-advertise-ipv6 里出现 '%' 直接拒绝（配置期响亮失败）
client 自己补 zone：复用 --rpc-ip 里的 zone；两端都缺则报错让用户补 %<if>
server 绑 link-local 必须带自己的 zone（缺了内核 EINVAL，配置期先给可读错误）
绑定带 zone 时通告回落成通配 sentinel（zone 不能上 wire）
```

单机回环测试时 zone 天然一致，所以"怎么试都通"不能证明跨机可用；功能测试优先用 ULA（`fd00::/8`）。
