#include "aqua/diagnostics/snapshot_line.h"

#include "aqua/logger/logger.h"

#include <exception>
#include <utility>

namespace aqua::diagnostics {

SnapshotLine::SnapshotLine(std::string component_name)
    : component_name_(std::move(component_name))
{
    aqua::log_debug_fmt("SnapshotLine created: component={}", component_name_);
}

void SnapshotLine::add_source(std::string name, SourceFn fn)
{
    // 先打日志再 move：避免为了一行 trace 而多抄一份 name。
    aqua::log_trace_fmt("SnapshotLine source registered: component={} source={}",
        component_name_, name);
    sources_.push_back(Source { std::move(name), std::move(fn) });
}

void SnapshotLine::log_debug() const
{
    if (!aqua::log_level_enabled(aqua::LogLevel::Debug)) {
        return;
    }

    std::string line = component_name_ + " diag:";
    for (const auto& source : sources_) {
        try {
            const auto value = source.fn();
            line.push_back(' ');
            line.append(source.name);
            line.push_back('{');
            line.append(value);
            line.push_back('}');
        } catch (const std::exception& e) {
            aqua::log_debug_fmt("{} diagnostics source '{}' failed: {}",
                component_name_, source.name, format_exception_message(e));
        } catch (...) {
            aqua::log_debug_fmt("{} diagnostics source '{}' failed: unknown exception",
                component_name_, source.name);
        }
    }

    if (aqua::log_level_enabled(aqua::LogLevel::Trace)) {
        aqua::log_trace_fmt("SnapshotLine assembled: component={} sources={}",
            component_name_, sources_.size());
    }
    aqua::log_debug(line);
}

} // namespace aqua::diagnostics
