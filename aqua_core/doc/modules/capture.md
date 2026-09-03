# 模块：AudioCapture / CaptureManager / WASAPI Capture

## 抽象

`AudioCapture` 是输入 push 接口：

```text
OS -> AudioBlock 回调
```

契约要点（见 `audio_capture.h` 头注释）：

- block 回调运行在 backend 的实时音频线程上，必须 RT-safe；
- `block.data` 仅在回调内有效；
- 回调签名 `noexcept`，异常不得越界；
- `start()` 成功前不会触发回调；`stop()` 返回时保证回调不再被调用（实现需 join 音频线程）；
- `stop()` 之后可以再次 `start()`，同一实例复用——这是设备切换的基础；
- 控制 API（`start` / `stop` / `is_running` / `info`）由同一控制线程调用；**禁止在 block / event 回调内调用它们**。

## CaptureManager

`CaptureManager`（`src/audio/capture/capture_manager.cpp`）托管采集流生命周期，是切换事务的唯一编排者。它不触碰
packetizer、网络与会话。

```text
ServerRuntime --> CaptureManager --> AudioCapture --> WASAPI
```

职责：

- **路由**：`config.device` 为空 = `FollowSystem`（跟随该 source 方向的系统默认）；有值 = `PreferredDevice`（sticky 意图，
  fallback 降级不覆盖）。
- **候选链**：`[目标设备, 先前的实际设备, 系统默认]` 去重，逐个尝试，首个成功即 Running；链耗尽 = `Fatal`。
- **格式不可变**：首流成功后把 `info().format` 钉进配置，后续候选以显式格式启动；不支持即视为该候选失败。
- **防抖**：错误驱动与默认跟随共用 10s / 3 次预算，超限直接 Fatal。
- **状态**：`CaptureSwitchState`（Inactive / Starting / Running / Switching / Fatal）。

候选的实际设备由 `AudioDeviceManager::resolve()` 解析后交给 backend（capture 后端没有设备回读接口），解析失败即该候选失败。
完整决议见 `../capture_switching_design.md`。

## WASAPI loopback

Server 默认 `OUTPUT_LOOPBACK`：数据来自系统 render engine 的混音，事件粒度不是 packetizer 需要的定长 F，因此
Capture → Packetizer 必须允许变长 block。

## 事件饥饿 fallback（欠账驱动的时间轴补偿）

WASAPI loopback 在最后一个 render client 消失后可能进入 quiescence，audio event 可能长期不触发；切歌等 render 流重建期间
engine 也可能反复 signal event 但不产出 packet（空事件），或零星吐小包（部分饥饿）。采集时间轴必须与 engine 的 event 行为
解耦，恒以 1x 墙钟速率推进——这是 capture 组件的契约：client 的 JitterBuffer 只负责网络抖动，不替采集端的停滞擦屁股。

每轮唤醒（事件或 20ms 超时）统一对账：

```text
expected  = 距上轮结算的墙钟欠账（含小数累积，防漂移）
balance  += expected - 本轮真实交付帧数
balance > 0  欠账：立即合成静音补齐（空事件 / 零星小包 / 完全静默由同一公式覆盖）
balance < 0  盈余：engine 暴发，留存抵扣未来欠账（避免迟到真实数据与已补静音重复计时）
```

连续 2 轮补偿（约 40ms 欠账）才把流级状态标成 `Starved`；第一轮欠账就生成合成静音，时间轴不停。单轮补偿上限约 150ms
（同时是盈余留存上限），防止系统从长时间挂起恢复后瞬间制造巨大 burst；超出上限的欠账丢弃。

`GetBuffer` 的 `AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY`（engine 官方断流信号，切歌的典型场景）只记录 debug 日志——墙钟欠账
模型已覆盖该窗口，无需特殊处理。

## 运行期错误

设备失效通过 event 回调（backend 的 event 线程）通知，回调内**只置标志，不做 stop/start**（那会 join 自己）。

| 错误                  | 去向                                                            |
|-----------------------|-------------------------------------------------------------------|
| `DeviceDisconnected`  | `ServerRuntime` 置切换待处理标志 → control tick 执行 restart 事务；**不置 Degraded** |
| 其它（后端内部错误）  | 置 `last_audio_error_`，runtime 迁 `Degraded` → CLI 停止          |

backend 自身不换设备：重建端点是 `CaptureManager` 的事务，backend 只保证"同一实例 stop 后可再次 start"。
