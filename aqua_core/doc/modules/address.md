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
