# 模块：AudioDeviceManager / WASAPI Devices

设备管理器的关键设计是“ **解析一次，stream 一次**”。

ServerRuntime 在构造期解析 capture device，并把同一 device id 用于：

1. default format probe；
2. 实际 capture start。

这样 system default device 在构造后变化不会让“探测格式”和“真正启动设备”落在两个 endpoint 上。

Client playback device 同样在 runtime start 路径解析；运行期不支持热切换。

WASAPI backend 还负责将 Windows endpoint id/name 映射成跨平台 `AudioDevice` 值对象；平台对象不跨 Core API 泄漏。
