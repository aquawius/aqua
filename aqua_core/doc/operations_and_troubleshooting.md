# Operations & Troubleshooting

## 1. 启动前检查

先执行：

```text
server --list-devices
client --list-devices
```

确认：

- server `loopback` 使用的 OUTPUT endpoint 存在；
- input capture 指向 INPUT endpoint；
- client playback endpoint 存在；
- 指定 device ID 来自当前系统，而不是旧机器保存的 ID。

## 2. 地址问题

`udp-ip` / `rpc-ip` 是本地 bind address，可以使用 `0.0.0.0` 或 `::`。

`advertise-ip` 是远端目的地址提示，可以使用 wildcard sentinel。Server CLI/Core 会拒绝无法解析的配置；Client 收到 wildcard、空值或非法 IP 时，为兼容/防御目的使用 `--server-ip` fallback。显式且可解析的 non-wildcard advertised IP 优先。

多网卡机器需要指定 client 实际可达的 UDP 地址时，应使用显式 `--advertise-ip`，不要依赖 wildcard fallback。

## 3. 音频格式问题

Server 省略 `--encoding/--channels/--sample-rate` 时，Runtime 先从 selected capture endpoint 查询 backend shared-mode 默认格式，再创建 packetizer/queue。capture 真正启动后会再次核对实际格式。

因此：

```text
backend probe != actual capture format
```

会被视为 startup failure，而不是让 network plane 在错误 geometry 下运行。

## 4. JitterBuffer 调试

正常性能运行不打开：

```text
AQUA_JITTER_BUFFER_RT_DEBUG_LOG
```

需要调查 FILL / DROP / REANCHOR 时，在 Debug 构建中显式开启。该开关会故意允许 RT 路径同步日志，因此只能用于诊断，不用于性能基准或发行运行。

## 5. 常见现象

### Client Connect 成功但 UDP 不通

检查：

1. server `udp-ip` 是否真正监听；
2. `advertise-ip` 是否与 client 路由一致；
3. server/client 是否跨 IPv4/IPv6 地址族；
4. firewall 是否允许 UDP data port。

### Server 启动但 capture 失败

检查设备 ID、设备方向与 backend 默认格式。loopback 选择 OUTPUT endpoint；input 选择 INPUT endpoint。

### 音频异常但网络统计正常

优先观察 Client diagnostics 中：

```text
jb_push_accepted
jb_push_rejected
jb_fill_episodes
jb_drop_episodes
jb_skip_slots
jb_reanchor
```

再考虑打开 `AQUA_JITTER_BUFFER_RT_DEBUG_LOG` 获取 episode 内部细节。
