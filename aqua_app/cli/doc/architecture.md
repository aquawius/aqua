# CLI 架构

CLI 是 Core 的宿主，不是第三套业务实现。

```text
main
 ├─ init_logger
 ├─ parse CLI
 ├─ create io_context
 ├─ create/start Runtime
 ├─ start diagnostics thread (dedicated, 非 io_context)
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

诊断快照在 **专用 `std::thread`** 上每秒运行（不占用 io_context worker，避免周期性卡住网络收发——这正是早期把它放在
io_context 上踩过的坑）；500ms control poll 才运行在 io_context 上。两者都是观察器，不是音频控制器。
