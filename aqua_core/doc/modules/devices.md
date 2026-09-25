# 模块：AudioDeviceManager / WASAPI / AAudio Devices

`AudioDeviceManager` 是设备系统的唯一入口：枚举、默认设备、按 id 解析、查询 shared-mode 默认格式。它不持有音频流，不触碰
采集/回放的实时线程。

```cpp
enumerate(direction)                      -> vector<AudioDevice>
default_device(direction)                 -> optional<AudioDevice>
default_format(direction, requested)      -> expected<AudioFormat, AudioError>
resolve(direction, requested)             -> expected<AudioDevice, AudioError>
```

`requested == nullopt` 表示"该方向的系统默认设备"。`resolve()` 是启动/切换路径，用 `expected` 区分
`DeviceNotFound` / `BackendFailed` 等失败原因。

## 解析与会话的关系

设备解析发生在三个时刻，作用各不相同：

| 时刻               | 谁在做                               | 用途                                                        |
|--------------------|--------------------------------------|-------------------------------------------------------------|
| ServerRuntime 构造 | `resolve()`                          | 探测格式，确定 packetizer / queue 几何（**只用于探测**）    |
| 采集/回放启动      | `CaptureManager` / `PlaybackManager` | 把 `AudioRoute`（`follow_system()` = 系统默认，`pin(id)` = 指定设备）解析成具体 endpoint |
| 设备切换事务       | 同上                                 | 逐个候选重新解析，首个成功者成为新的实际设备                |

也就是说： **构造期解析出的 device id 不会钉住运行期的流**。系统的默认设备在会话期间变化是正常情况，由切换管理器按路由策略
处理，而不是静默改变流几何——几何（`AudioFormat` 与 F）在会话内恒定。

## 路由词汇（同目录的两个值类型）

`audio/devices/` 除设备管理器外还放着路由的两根正交轴，两侧共用同一份（决议见
`../playback_switching_design.md` §4 与 §16.1）：

- `audio_route.h` —— `AudioRoute` / `AudioRouteAuthority`：给 backend 的 **请求**（路由权归平台还是归应用）。
  `AudioPlaybackConfig` 与 `AudioCaptureConfig` 的设备选择 **只有** `route` 这一个字段。
- `route_policy.h` —— `RoutePolicy` / `RouteIntentOwner` / `RouteLossAction`：manager 的 **决策状态**
  （意图归谁、钉住的设备丢了怎么办）。它取代了原先分居 `audio/playback/` 与 `audio/capture/` 的
  `playback_route_mode.h` / `capture_route_mode.h`（两者均已删除）。

设备 id 本身是平台/会话内的实现细节（跨会话不稳定、不可比较），所以它只作为 `Application` 权威下的载荷出现， 不是独立的请求维度。

## 方向

`AudioDeviceDirection`：`INPUT`（麦克风等）/ `OUTPUT`（扬声器、耳机等）。采集的 loopback 不是独立设备类型，而是 source：
`OUTPUT_LOOPBACK` 在 OUTPUT 方向解析，`INPUT_DEVICE` 在 INPUT 方向解析，两者绝不混向解析。

## WASAPI 实现

`WasapiAudioDeviceManager` 负责把 Windows endpoint id / friendly name 映射成跨平台 `AudioDevice` 值对象，平台对象不跨
Core API 泄漏：

- `enumerate()` 只返回 `DEVICE_STATE_ACTIVE` 设备，并标记系统默认那一项；
- `resolve()` 对显式 id 会校验方向一致性，方向不符按 `DeviceNotFound` 处理；
- `default_format()` 通过 `IAudioClient::GetMixFormat()` 取得该设备的 shared-mode 格式，不启动音频流。

每次调用各自做 COM 初始化（`ScopedComInitialization`），不长期持有 enumerator，也不注册设备通知回调——设备变化由上层轮询
或平台推送发现（见 `../capture_switching_design.md` §6 与 `../playback_switching_design.md` §5 rev2 / §17）。

## DeviceSetPoller（轮询去抖）

`aqua/audio/devices/device_set_poller.h`，平台无关，位于 `aqua_core_base`。

把「每 control tick（500ms）枚举一次」收敛成「确认过的变化才上报」，供两个 manager 的 `tick()` 第 1 步使用：

```text
poll() -> std::optional<DeviceSetChange>   // nullopt = 本次无可上报的变化
DeviceSetChange { present, added, removed } // present 按 id 有序；首次上报是基线，added/removed 皆空
reset()                                    // 新会话：丢弃采样/去抖/基线
```

两条约束（详见 `../playback_switching_design.md` §17.3）：

- **去抖**：变化必须连续 `kConfirmations` = 2 次采样一致才上报。WASAPI 在设备转换期可能瞬时枚举不全，
  单次采样就决策会 restart 一条健康的流。枚举顺序不作判据（排序后比较）。
- **无信息门控**：采样中不含任何非空 id ⇒ 返回 `nullopt`，且**不冲掉已有基线**。
  `AAudioAudioDeviceManager::enumerate` 只返回一条空 id 的合成条目，若当成真相，Android 上每个 tick
  都会判定活跃设备缺席。

`format_device_ids()` 是同头文件里的诊断格式化助手（`[a, b]`），供 debug 日志打印检测结果。

## AAudio 实现（Android）

`AAudioAudioDeviceManager` 是最小实现（决议见 `../android_roadmap.md` §6）：

```text
enumerate     只返回系统默认输入/输出两个设备
resolve(INPUT, nullopt)  -> 系统默认输入
resolve(OUTPUT, nullopt) -> 系统默认输出（id 为空字符串）
resolve(OUTPUT, "android:N") -> 放行（N = Java 层 AudioManager 的 int id，且必须 >= 0；
  "android:-1" 等负数拒绝为 DeviceNotFound，避免与 AAUDIO_UNSPECIFIED 语义碰撞）
resolve(OUTPUT, 其它格式) -> DeviceNotFound
resolve(INPUT, *) -> NotSupported（capture 阶段前不开放）
```

设备 id 形如 `android:N`（N 为 `AudioDeviceInfo` 的 id），由该 manager 编解码。Android 的设备路由（蓝牙耳机、USB 声卡插入）
由系统自动完成，Core 不维护 framework 侧的设备状态；路由变化表现为 stream error/disconnect，或由 Kotlin 层的
`AudioDeviceMonitor` 推送设备快照经 C API 交给 `PlaybackManager::on_devices_changed()` 决策。

注意 `enumerate()` 返回的是**空 id** 的合成条目，因此 `DeviceSetPoller` 在 Android 上恒判定「无信息」而不上报——
轮询路径自动让位给推送路径，两者不会同时喂 `on_devices_changed()`。
