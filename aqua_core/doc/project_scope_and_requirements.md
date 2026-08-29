# 项目范围与要求

## 目标

Aqua 是低延迟局域网音频共享系统：Server 从本机音频 endpoint 获取 PCM，Client 通过 gRPC 建立 session，通过 UDP 收到音频，通过 JitterBuffer 重排并播放。

## 当前功能

- Windows/WASAPI loopback capture
- Windows/WASAPI playback
- gRPC Connect/Disconnect
- UDP Audio / HELLO / HELLO_ACK
- session 超时回收
- IPv4/IPv6 literal address handling
- JitterBuffer pre-roll、Fill、Drop、缺帧静音、reanchor
- loopback 静默（quiescence）时的合成静音保帧（事件饥饿 fallback）
- capture 运行态诊断（Active/Silent/Starved 与相关计数）
- CLI 设备枚举和运行诊断

## 当前非目标

- 音频压缩 codec
- NAT traversal
- 公网安全协议
- Windows Exclusive
- 多级播放缓冲
- runtime 内热切换设备/格式
- 客户端隐式 resampling
- Android capture/loopback（Android 规划阶段明确为 playback first）

## 产品不变量

1. Server 一次运行固定格式与 F。
2. Client 必须按 Server 格式创建播放链路。
3. 一个 AudioFrame 对应一个完整 UDP Audio datagram。
4. playback callback 必须始终得到可播放输出，缺数据以静音补齐。
5. RT 路径不允许阻塞/分配/同步 I/O。
