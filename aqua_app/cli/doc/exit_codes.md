# CLI 解析与退出码

`ParseOutcome`：

```text
Run         -> 继续启动 Runtime
Help        -> 打印 usage，exit 0
ListDevices -> 列设备，exit 0
Error       -> 参数错误，exit 1
```

Runtime 启动失败由 main 以 fatal 日志报告并返回 1。

运行中 signal/Degraded 走正常 stop 路径，成功 teardown 后返回 0；Core 的 `stop()` 是否被调用多次不影响退出流程。
