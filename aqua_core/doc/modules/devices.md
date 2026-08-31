# 模块：AudioDeviceManager / WASAPI / AAudio Devices

设备管理器的关键设计是“ **解析一次，stream 一次**”。

ServerRuntime 在构造期解析 capture device，并把同一 device id 用于：

1. default format probe；
2. 实际 capture start。

这样 system default device 在构造后变化不会让“探测格式”和“真正启动设备”落在两个 endpoint 上。

Client playback device 同样在 runtime start 路径解析；运行期不支持热切换。

WASAPI backend 还负责将 Windows endpoint id/name 映射成跨平台 `AudioDevice` 值对象；平台对象不跨 Core API 泄漏。

## AAudio DeviceManager（Android）

`AAudioAudioDeviceManager` 是最小实现（决议见 `../android_roadmap.md` §6）：

```text
enumerate     只返回系统默认输入/输出两个设备
resolve(INPUT, nullopt)  -> 系统默认输入
resolve(OUTPUT, nullopt) -> 系统默认输出
resolve(*, 显式 id)      -> 拒绝（NotSupported）
```

设备路由（蓝牙麦/耳机插入）由系统自动完成，Core 不维护 Android framework 设备状态；路由变化表现为
stream error/disconnect，走 AAudio error callback 的 pending error 路径。
