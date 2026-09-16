# Aqua CLI 文档

当前 CLI 有两个程序：

```text
aqua_server_cli
    capture -> packetize -> UDP broadcast

aqua_client_cli
    gRPC/UDP receive -> JitterBuffer -> playback
```

CLI 只负责：参数解析、Core 对象创建、诊断 timer、signal 和退出码。音频/网络业务全部在 `aqua_core`。

## Server

建议阅读顺序：`server.md` → `diagnostics.md`。

## Client

建议阅读顺序：`client.md` → `diagnostics.md`。

CLI 参数以 parser 源码为准，不应该把旧 Android UI 参数当成当前 CLI 配置模型。

## 开发约定（原 developer.md）

1. parser 只生成 `RuntimeConfig`，不创建 runtime。
2. main 负责：logger init、parse、runtime create/start、diagnostics、signal、stop。
3. 不在 CLI main 实现 UDP/gRPC/audio 业务。
4. 新配置先加入 Core config，再在 CLI parser 映射；不要在 CLI 偷加第二套语义。
5. 任何新参数都必须说明单位、默认值、范围以及是否会改变协议/RT 语义。

## 解析结果与退出码（原 exit_codes.md）

`ParseOutcome`：

```text
Run         -> 继续启动 Runtime
Help        -> 打印 usage，exit 0
ListDevices -> 列设备，exit 0
Error       -> 参数错误，exit 1
```

Runtime 启动失败由 main 以 fatal 日志报告并返回 1。运行中 signal/Degraded 走正常 stop 路径，
成功 teardown 后返回 0；Core 的 `stop()` 是否被多次调用不影响退出流程。
