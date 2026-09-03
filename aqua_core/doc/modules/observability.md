# 模块：Logger / Diagnostics

## Logger

统一经 spdlog。sink 按平台选择：Windows / Linux 用 `stdout_color_sink_mt`；Android 用 `android_sink_mt`，tag 为 `aqua`
（Android 上 native stdout 不是可靠的日志输出）。

级别：`Trace` / `Debug` / `Info` / `Warn` / `Error` / `Fatal`，默认 `Info`。解析时接受别名（`warning` / `critical`）。

级别设置途径：

- CLI：`--log-level <name>`（启动时一次性设定）；
- C API：`aqua_client_config` 的 `log_level` 字段，以及 `aqua_set_log_level()`。

**没有环境变量入口**，也没有运行时命令：CLI 启动后不能再改。日志文本统一为 UTF-8；Windows 的 system error 经
`FormatMessageW` + UTF-8 转换归一化，避免 ACP 乱码。

## Diagnostics

`Diagnostics` 不拥有 runtime state，只注册 getter 并在快照时读取：

- `add_source(name, fn)`：返回一行字符串快照；
- `add_counter(name, fn)`：按快照间隔输出 `total` / `delta` / `rate`，rate 用真实 elapsed 计算，不假定定时器精确。

Debug 未启用时 `log_debug()` 直接返回，连 source 都不会调用——诊断 getter 跨多个 atomic 读取，不看 debug 日志就不该付这份
成本。

CLI 以 1s 周期打印；Android App 以 1s 节流（500ms 轮询 + 每两次取一次诊断）。

## RT 日志

正常 realtime callback 不写日志。两处例外：

- `AQUA_JITTER_BUFFER_RT_DEBUG_LOG`：开发期观察水位 / episode / reanchor，**开启后破坏 RT 契约**；
- WASAPI 采集循环在 DATA_DISCONTINUITY 分支的日志、以及回放错误路径的日志——已知偏差，故障时会成簇触发 spdlog 锁竞争。

两者都只能短时间复现问题时开启，不得作为生产基线。
