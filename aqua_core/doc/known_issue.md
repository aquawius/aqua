# 已知取舍（known issues）

本文记录**设计之初有意接受**的取舍：现象、放弃的另一边、接受的理由、重开条件。
行为描述以代码为准；数值以 `buffer_config.h` / `udp_config.h` / 各 manager 头文件为准。
与故障排查手册的关系：`operations_and_troubleshooting.md` 回答"坏了怎么看"，
本文回答"为什么当初这样定"。

## 1. 切换链末重试阻塞 ioc 最长 2s

`PlaybackManager::switch_to` 在候选链耗尽且末错误瞬时（`DeviceUnavailable` /
`DeviceDisconnected` / `BackendFailed`）时，同步重试上一设备（4 次，退避
200ms×n，累计约 2s）。错误驱动恢复跑在 ioc 线程，这 2s 内 UDP 心跳停摆，吃掉约
40% 的 5s 会话预算。

接受理由：这是"换设备别把整条连接带崩"的最后一道保险；可达窄——`DeviceNotFound` /
`FormatUnsupported` 直接跳过重试，真拔设备不走这条路；2s 仍在 5s 预算内。
重开条件：出现心跳被饿死导致的 Degraded 实测，或做 pending-retry 状态机
（记截止时刻后返回，由 supervision tick 再驱动——改动涉及吸收点时序，需独立评审）。

## 2. 跟随与错误共享 10s/3 预算

错误驱动 restart、内部跟随（tick/快照/自动切回）合并计数，风暴期打满即 `Fatal`。
路由抖动已由独立 settle 预算分流，不烧这里的额度；1s 合并窗口先吸收蓝牙连接风暴。
重开条件：出现"正常插拔风暴被判 Fatal"的实测。

## 3. `AAUDIO_ERROR_UNAVAILABLE` 归入 `BackendFailed`

该码语义含混（设备不可用 vs 请求配置不可用），现判不可恢复（→ `Degraded` 停会话），
而非 `DeviceUnavailable`（→ 恢复）。切换路径不受影响（瞬时重试三码同权），差异只在
error 回调这一条路上。接受理由：若真表示配置不可用，重试只是无谓打转；error 回调日志
已同时打印原始码，可观测。重开条件：实测抓到该码对应可恢复场景。

## 4. settle 常量来自单一样本

`kRouteSettleWindow` 400ms / `kSettleRetryInterval` 200ms / `kMaxSettleRestarts` 8 /
`kSettleWindow` 5s 由一份蓝牙切换日志定标。接受理由：有实测总比拍脑袋强，且降级路径
（耗尽转系统默认）兜住了"样本外更长的路由转换"。重开条件：多机型实测中"stream died Nms"
 routinely 超过 400ms——只加宽窗口，其余不动（测试 `SettleExhaustionDegrades…` 锁定形状）。

## 5. Android 全集推送的跟随副作用

`present_ids` 是未过滤 sink 全集（`active_gone` 判断必需），而 Android 的
`default_device` 是合成空 id，于是 `None` 归属下**任何**新 sink 到达即跟随——含 SCO
等通话类型。接受理由：单次多余 restart（预算封顶），远好于子集推送造成的每快照误判
`Fatal` 循环；core 拿不到设备类型（id 是不透明 `android:N`），分不清媒体与通话路由。
重开条件：JNI 透传设备类型（C 边界变更）或系统路由可查询。

## 6. 空集与枚举失败不可分

`DeviceSetPoller` 依据原始项是否含空 id 区分"合成条目（无信息）"与"真零设备（有效空集）"，
但 WASAPI `enumerate()` 以返回值区分不了"真零设备"与"COM/枚举失败"——两者都是空向量。
接受理由：失败连两次才上报（去抖），偶发失败几乎到不了决策；真零设备必须能上报（否则
capture"主动 Fatal"保证失效）。重开条件：`enumerate()` 改 `expected`（动接口与全部 mock）。

## 7. capture 只有 `DeviceDisconnected` 触发切换

运行期 `BackendFailed`（如 `SERVICE_NOT_RUNNING`）走 `Degraded` 停 server，不进
restart。接受理由：后端服务死亡不是换个端点能解决的，重试只会拖长无声。重开条件：出现
可恢复的 `BackendFailed` 实测。

## 8. session_id 不是每会话强随机

`thread_local mt19937_64`（`random_device` 播种一次），输出可逆。接受理由：威胁模型只有
局域网被动观察者；注释与协议文档已如实说明，不再声称 CSPRNG。重开条件：威胁模型升级
（公网/主动攻击者）——届时换系统熵源逐 id 生成。

## 9. 空/非法 UDP 通告静默回退

ConnectResponse 的空串/非法地址回退到 gRPC `server_ip`（不断连）；端口 0/超限仍拒绝。
接受理由：server 故障或中间件篡改时保活优先；端口错误必是配置错，拒得越响越好。
单测锁定回退成功路径（此前两用例名不副实：因缺格式而拒却挂地址的名，已修正）。

## 10. `UdpClient::open` 的 IPv4 死分支

`start_receive` 在 remote 已设但未 open 时硬编码 `0.0.0.0:0` 打开，而 `set_remote` 按远端
地址族选择 wildcard。接受理由：当前调用序下 `set_remote` 已隐式 open，该分支不可达；
IPv6 `UdpClient` 用例缺失（全是 `127.0.0.1`）。重开条件：调用序变化或补 IPv6 用例时一并清理。

## 11. `connected_scratch_` 单调用方假设

`UdpServer` 复用成员 scratch 做快照，无锁、无断言，仅靠注释约束"同一时刻只有一个
dispatcher/owner"。接受理由：现全仓唯一调用方，无并发广播测试是缺口而非 bug。重开条件：
第二个 broadcast 调用方出现——先加断言/锁再接。

## 12. Kotlin 容错优先于排障信号

`AquaRouteMode.fromCode` 等遇未知码回落 benign 默认；C 侧查询入口已加 magic 浅校验，
JNI 非法句柄传 null。接受理由：App 不因诊断抖动断连；野句柄在 C 侧拦，Kotlin 侧显示保守值。
重开条件：排障需要时把回落改成"未知"标签（加枚举值，C/Kotlin/诊断三处同步）。

## 13. 重连固定 3s 退避、无上限

`RECONNECT_DELAY_MS` 统一快/慢失败节奏，保证"已停止"可见；无指数退避、无次数上限。
接受理由：core 契约"终态即停"，重连是 UI 层行为；停止期可见优先于退避优雅性，用户断开即停。
重开条件：自动重连风暴实测（如 server 长时间不可达时的行为投诉）。

## 14. 保留项（非问题，防后人"顺手"删改）

- `PlaybackManager::restart()` 无生产调用者：测试覆盖的挂载点，删了省不下什么，留给将来 GUI 重连。
- `AudioRoute{Application, nullopt}` 构造即静默等价跟随系统：三处边界（C API / 双 CLI）均拒绝空 id，
  构造不出来；`follows_system()` 访问器语义不闭合但零调用，动它不如不动。
- S_FALSE 丢弃 `closest_match`（playback）与 capture 侧 `!= S_OK` 拒绝：差异是刻意的（播放端引擎转换
  不改变会话语义，采集端重采样违反格式不可变），不要"顺手统一"。
- 回调超额返回帧 → `InvalidArgument` → `Degraded`：`JitterBuffer::pull` 受 `output.size()/frame_bytes_`
  限死，构造上不可达；响亮失败好过静默 clamp。
- 跟随系统 + 无关设备到达不重开、默认已跟上不再切：`UnrelatedDeviceArrival…` /
  `DefaultFollowAndSetChangeDoNotDoubleSwitch` 锁定，轮询 2Hz 下的必要约束。
