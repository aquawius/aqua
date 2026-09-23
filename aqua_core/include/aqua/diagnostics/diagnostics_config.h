#ifndef AQUA_DIAGNOSTICS_DIAGNOSTICS_CONFIG_H
#define AQUA_DIAGNOSTICS_DIAGNOSTICS_CONFIG_H

// 诊断层的节奏常量（唯一一处；数值口径见 configuration_reference.md）。
//
// 本文件只放"多久采一次/打一次"这类节奏，不放字段语义（字段契约在
// client_diagnostics_snapshot.h / server_diagnostics_snapshot.h，读法在
// doc/diagnostics.md，日志点位在 doc/modules/observability.md）。
//
// 为什么单独成文件而不是留在 runtime_config.h：诊断是独立关注点，它的节奏
// 被 CLI 的两个 main 使用，与 runtime 的生命周期无关；混在 runtime_config.h
// 里会让"改诊断密度"看起来像在动运行期参数。

#include <chrono>

namespace aqua::config {

// 诊断快照与输出节奏：CLI 的 diag 线程按此周期刷新一次聚合快照并打一行。
// 与运行期的 control poll（RUNTIME_CONTROL_POLL_INTERVAL，500ms）是两条独立
// 的节奏：poll 负责终态裁决，本值只负责"多久看一眼"。
inline constexpr std::chrono::milliseconds DIAGNOSTICS_SNAPSHOT_INTERVAL { 1000 };

} // namespace aqua::config

#endif // AQUA_DIAGNOSTICS_DIAGNOSTICS_CONFIG_H
