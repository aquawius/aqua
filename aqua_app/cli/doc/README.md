# Aqua CLI 文档

当前 CLI 有两个程序：

```text
aqua_server
    capture -> packetize -> UDP broadcast

aqua_client
    gRPC/UDP receive -> JitterBuffer -> playback
```

CLI 只负责：参数解析、Core 对象创建、诊断 timer、signal 和退出码。音频/网络业务全部在 `aqua_core`。

## Server

建议阅读顺序：`server.md` → `diagnostics.md`。

## Client

建议阅读顺序：`client.md` → `diagnostics.md`。

CLI 参数以 parser 源码为准，不应该把旧 Android UI 参数当成当前 CLI 配置模型。
