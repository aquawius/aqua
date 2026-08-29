# Aqua

Aqua 是一个低延迟、局域网优先的音频共享系统：Server 从本机音频端点采集 PCM，经 gRPC 建立 session 后使用 UDP 广播定长音频帧；Client 接收 UDP、在单一 JitterBuffer 中完成乱序/丢包/水位处理，再交给本地回放后端。

当前工程基线：**0.2.0**。

## 当前实现状态

| 模块 | Windows/WASAPI | Linux | macOS | Android |
|---|---|---|---|---|
| Device enumeration | 已实现 | 接口预留 | 接口预留 | 接口预留 |
| Server capture | 已实现 | 未实现 | 未实现 | 未实现 |
| Loopback capture | 已实现 | 未实现 | 未实现 | 未实现 |
| Client playback | 已实现 | 未实现 | 未实现 | 未实现 |
| gRPC control plane | 已实现 | 接口可用 | 接口可用 | 独立工程规划 |
| UDP data plane | 已实现 | 已实现 | 已实现 | 核心可复用 |
| JitterBuffer | 已实现 | core 已实现 | core 已实现 | core 已实现 |

## 快速使用

Server 不需要任何参数即可使用默认配置启动：

```text
aqua_server
```

默认配置：`0.0.0.0:50051` 提供 gRPC，`0.0.0.0:9999` 提供 UDP；使用系统默认 OUTPUT endpoint 做 loopback capture；音频格式直接采用该 endpoint 的 backend 默认格式。

查询设备：

```text
aqua_server --list-devices
aqua_client --list-devices
```

Server 默认就是系统默认 OUTPUT 的 loopback。也可以直接使用 `--device-id <OUTPUT_ID>` 指定一个输出设备启动 loopback；若要采集 INPUT 设备，再使用 `--capture=input --device-id <INPUT_ID>`。Client 用 `--device-id` 指定 playback OUTPUT endpoint。设备列表同时显示 backend 默认格式。

Server 格式只有在用户明确提供以下三项时才被 override：

```text
--encoding <s16|s24|s32|f32|u8> --channels <N> --sample-rate <Hz>
```

否则不调整格式，直接采用所选 capture endpoint 的 backend default。

Client 最小启动命令只有两个参数：

```text
aqua_client --server-ip <SERVER_IP> --server-rpc <RPC_PORT>
```

## 设计文档

完整设计入口位于 `aqua_core/doc/README.md`，内容覆盖：需求、架构、音频、Buffer、协议、线程与生命周期、设备与格式、诊断、测试、构建发布、安全部署及冻结决策。
