# aqua_client

## 最简启动

Client 最少只需要 Server IP：

```text
./aqua_client_cli --server-ip 192.168.1.10
```

默认配置为：

```text
Server gRPC：<server-ip>:50051
UDP：从 gRPC Connect 响应获取
回放设备：系统默认 OUTPUT 设备
回放格式：使用 Server 返回的音频格式
JitterBuffer：30 slots
Client 名称：aqua-client
```

Client 不需要手动指定 UDP 端口；Server 会在 gRPC Connect 响应中提供 UDP endpoint。Server 默认使用 `0.0.0.0:50000`
监听，因此正常部署时 Client 只需要知道 Server 的可达 IP。

## 参数

```text
--server-ip            必填，Server 的可达 IPv4/IPv6 地址（不解析 DNS 主机名）
--rpc-port             Server gRPC 端口（Connect/Disconnect/Keepalive），默认 50051
--udp-force-port       覆盖 Server 下发的 UDP 端口；省略=使用 Server 通告的端口
--client-name          Client 名称，默认 aqua-client（1..128 字节）
--jb-capacity          JitterBuffer 容量（slots），默认 30，范围 4..512。
                       1 slot = 1 个 UDP 音频包（180 帧/48k 下 3.75ms），30 slots ≈ 112ms。
                       自适应 target 上限 = 2/3×N，超出部分买到的是抖动余量而非延迟；
                       512 是 reanchor O(N) 扫描的 RT 护栏
--jb-jitter-gain       自适应 target 的 k（margin = max(k×J, min(stall峰值+1包, 8槽))），默认 5。延迟↔稳定主力旋钮：
                       每 +1 ≈ 多 J/packet_ms 槽（180 帧/48k 下约 1.2 槽 ≈ 4.5ms）。
                       调到很大也不会失控：target 被 2/3×capacity 结构上限接住；
                       负值 / NaN / Inf 不报错，静默退回默认 5（TargetController 内）
--jb-min-target        target 硬下限（slots），默认 3。有效下限 =
                       max(本值, 几何地板 + 1)：地板无条件托底，只能抬高；
                       高于 capacity 时会被钳到容量（CLI 打 soft warning 提醒）
--jb-stall-peak-cap    stall 峰值项上限（slots），默认 8。决定一次孤立大 stall 最多
                       把 target 推多高（8 槽 ≈ 30ms @3.75ms 包）。
                       0 = 关闭 stall 峰值项（margin 退回纯 k×J）；负值 = 默认
--jb-stall-decay       stall 峰值衰减速度（ms/s），默认 10。"峰值记多久"：
                       调小 = 稀疏 stall 也记得住但大事故挂更久；调大 = 更快遗忘。
                       0 = 峰值永久保持（"历史最坏间隙"，极值实验）；负值 = 默认
--jb-stall-threshold   stall 门阈值（包周期倍数），默认 5。到达间隔超过它即判
                       断流、不计入 J。≤ 0 = 关检测（每个间隔都进 J，裸 RFC 3550，
                       验证 stall 剔除效果的唯一 A/B 手段）
--jb-underrun-penalty  每次欠载事件抬升 target 下限的槽数，默认 1
                       （累计上限 6 槽，0.5 槽/s 回落）。0 = 关闭整条欠载反馈闭环
                       （分离预测项/反馈项贡献的关键对照）；负值 = 默认
--jb-fixed-target      关闭自适应 target，回固定 target=0.60N / startup=0.50N
--jb-no-conceal        关闭 PCM concealment：缺帧直接静音（v1 行为）
--playback-device-id   OUTPUT 回放设备 ID；省略=系统默认 OUTPUT 设备
--log-level            trace|debug|info|warn|error|fatal
--list-devices         列出 OUTPUT 设备后退出
--version              显示版本后退出
--help                 显示帮助
```

已无 CLI 入口的 JB 参数（起步 target / 回落限速 / 涨后锁跌 / 死区 / 反馈累计 上限与回落速率 / conceal
连续上限）：它们只有一个很窄的合理区间，暴露出去只会 制造误调。默认值与取值理由集中在
`aqua_core/include/aqua/audio/buffer/buffer_config.h`，改那里重编译即可 （候选清单见 configuration_reference.md §5.2）。

参数原理、控制律推导与调参 playbook 详见
`aqua_core/doc/jitter_buffer_control_design.md`（§8 参数手册 / §9 playbook / §10 实验复现矩阵）；日志点位见
`aqua_core/doc/modules/observability.md`。

## 设备语义

Client 的 `--playback-device-id` 始终表示 OUTPUT 回放 endpoint。显式指定时，CLI 会尽早检查该 ID 是否能够解析为 OUTPUT
设备；不要传入 INPUT 设备 ID。

## 音频格式

Client 不在 CLI 中指定采样率、声道数和编码。gRPC Connect 返回的 Server AudioFormat 是整个会话的权威格式，Client 使用该格式创建
JitterBuffer 和回放流。

因此 Client 使用系统默认回放设备时，也必须保证该设备能够播放 Server 提供的格式。

## Server 地址与 UDP

Client 只负责提供 Server IP 和可选的 gRPC 端口。UDP 默认完全采用 Server 通过 gRPC 下发的地址端口；`--udp-force-port`
仅覆盖端口，不覆盖地址，主要用于 NAT/端口映射等场景。

连接成功后：

```text
Server gRPC
→ Connect
→ 获取 session_id / UDP endpoint / AudioFormat / F
→ 选择 UDP endpoint（默认 Server 通告端口；指定 `--udp-force-port` 时仅替换端口）
→ 接收 AudioFrame
```

当 Server 通告的是 `0.0.0.0` 或 `::` 时，Client 使用 `--server-ip` 作为 UDP 目标 IP。Client 不提供 `force IP` 覆盖项；Server
应通过 `--udp-advertise-ip` 正确提供客户端实际可达的 UDP 地址。

## 退出

```text
停止回放
→ 停止 UDP
→ 最佳努力发送 Disconnect
→ Runtime 停止
→ 进程退出
```
