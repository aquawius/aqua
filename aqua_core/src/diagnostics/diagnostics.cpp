#include "aqua/diagnostics/diagnostics.h"

#include "aqua/logger/logger.h"

#include <exception>
#include <utility>

namespace aqua::diagnostics {

Diagnostics::Diagnostics(std::string component_name)
    : component_name_(std::move(component_name))
{
    aqua::log_debug_fmt("Diagnostics created: component={}", component_name_);
}

void Diagnostics::add_source(std::string name, SourceFn fn)
{
    const auto source_name = name;
    sources_.push_back(Source { std::move(name), std::move(fn) });
    aqua::log_trace_fmt("Diagnostics source registered: component={} source={}",
        component_name_, source_name);
}

void Diagnostics::log_debug() const
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
                component_name_, source.name, e.what());
        } catch (...) {
            aqua::log_debug_fmt("{} diagnostics source '{}' failed: unknown exception",
                component_name_, source.name);
        }
    }

    if (aqua::log_level_enabled(aqua::LogLevel::Trace)) {
        aqua::log_trace_fmt("Diagnostics snapshot assembled: component={} sources={}",
            component_name_, sources_.size());
    }
    aqua::log_debug(line);
}

} // namespace aqua::diagnostics
