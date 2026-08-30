# 模块：Logger / Diagnostics

## Logger

统一经 spdlog default logger。Windows 使用 stdout color sink；Android 使用 `android_sink`，tag=`aqua`，因为 app stdout 在
Android 上不能作为可靠 native log 输出。

日志文本契约是 UTF-8。Windows system error 通过 FormatMessageW + UTF-8 conversion 归一化，避免 ACP 乱码。

默认级别 Info；CLI 可以显式设置 Trace/Debug/Warn/Error/Fatal。

## Diagnostics

Diagnostics 不拥有 runtime state，只注册 getter 并在 snapshot 时读取。CLI 以 1s 周期打印 debug snapshot。

Counter rate 是基于真实 elapsed time 的 delta rate，而不是“假定正好 1 秒”。

## RT 日志

正常 realtime callback 不应该直接写日志。JitterBuffer 的 RT debug 宏是刻意例外，只用于开发期观察水位/episode/reanchor。
