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

| 时刻                | 谁在做                        | 用途                                                   |
|---------------------|-------------------------------|----------------------------------------------------------|
| ServerRuntime 构造  | `resolve()`                   | 探测格式，确定 packetizer / queue 几何（**只用于探测**） |
| 采集/回放启动       | `CaptureManager` / `PlaybackManager` | 把路由（空 = 系统默认，有值 = 指定设备）解析成具体 endpoint |
| 设备切换事务        | 同上                          | 逐个候选重新解析，首个成功者成为新的实际设备             |

也就是说：**构造期解析出的 device id 不会钉住运行期的流**。系统的默认设备在会话期间变化是正常情况，由切换管理器按路由模式
处理，而不是静默改变流几何——几何（`AudioFormat` 与 F）在会话内恒定。

## 方向

`AudioDeviceDirection`：`INPUT`（麦克风等）/ `OUTPUT`（扬声器、耳机等）。采集的 loopback 不是独立设备类型，而是 source：
`OUTPUT_LOOPBACK` 在 OUTPUT 方向解析，`INPUT_DEVICE` 在 INPUT 方向解析，两者绝不混向解析。

## WASAPI 实现

`WasapiAudioDeviceManager` 负责把 Windows endpoint id / friendly name 映射成跨平台 `AudioDevice` 值对象，平台对象不跨 Core
API 泄漏：

- `enumerate()` 只返回 `DEVICE_STATE_ACTIVE` 设备，并标记系统默认那一项；
- `resolve()` 对显式 id 会校验方向一致性，方向不符按 `DeviceNotFound` 处理；
- `default_format()` 通过 `IAudioClient::GetMixFormat()` 取得该设备的 shared-mode 格式，不启动音频流。

每次调用各自做 COM 初始化（`ScopedComInitialization`），不长期持有 enumerator，也不注册设备通知回调——设备变化由上层轮询
或平台推送发现（见 `../capture_switching_design.md` §6 与 `../playback_switching_design.md` §5 rev2）。

## AAudio 实现（Android）

`AAudioAudioDeviceManager` 是最小实现（决议见 `../android_roadmap.md` §6）：

```text
enumerate     只返回系统默认输入/输出两个设备
resolve(INPUT, nullopt)  -> 系统默认输入
resolve(OUTPUT, nullopt) -> 系统默认输出
resolve(*, 显式 id)      -> 拒绝（NotSupported）
```

设备 id 形如 `android:N`（N 为 `AudioDeviceInfo` 的 id），由该 manager 编解码。Android 的设备路由（蓝牙耳机、USB 声卡插入）
由系统自动完成，Core 不维护 framework 侧的设备状态；路由变化表现为 stream error/disconnect，或由 Kotlin 层的
`AudioDeviceMonitor` 推送设备快照经 C API 交给 `PlaybackManager::on_devices_changed()` 决策。
