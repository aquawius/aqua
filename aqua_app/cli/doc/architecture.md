# CLI 架构

CLI 是 Core 的宿主，不是第三套业务实现。

```text
main
 ├─ init_logger
 ├─ parse CLI
 ├─ create io_context
 ├─ create/start Runtime
 ├─ install diagnostics timer
 ├─ install control poll
 ├─ install signal handler
 ├─ io_context.run()
 └─ stop Runtime
```

## Parser

Parser 只做：

```text
argv -> typed RuntimeConfig + LogLevel + ParseOutcome
```

不能在 parser 里启动网络/音频。

## main

main 是进程级 composition root：它决定“什么时候退出”，Runtime 决定“业务如何运行”。

## 诊断

诊断 timer 在同一个 `io_context` 中周期运行，但不会参与 audio callback。control poll 也是观察器，不是音频控制器。
