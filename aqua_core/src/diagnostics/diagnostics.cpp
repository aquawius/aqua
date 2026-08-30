#include "aqua/diagnostics/diagnostics.h"

#include "aqua/logger/logger.h"

#include <exception>
#include <format>
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

void Diagnostics::add_counter(std::string name, CounterFn fn)
{
    struct State {
        std::uint64_t last = 0;
        std::chrono::steady_clock::time_point last_sample =
            std::chrono::steady_clock::now();
        bool initialized = false;
    };
    const auto state = std::make_shared<State>();
    add_source(std::move(name), [state, fn = std::move(fn)]() mutable {
        const auto now = std::chrono::steady_clock::now();
        const auto total = fn ? fn() : 0;
        std::uint64_t delta = 0;
        double rate = 0.0;
        if (state->initialized) {
            delta = total >= state->last ? total - state->last : 0;
            const auto elapsed = std::chrono::duration<double>(now - state->last_sample).count();
            if (elapsed > 0.0) {
                rate = static_cast<double>(delta) / elapsed;
            }
        } else {
            state->initialized = true;
        }
        state->last = total;
        state->last_sample = now;
        return std::format("total={} delta={} rate={:.2f}/s", total, delta, rate);
    });
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
                component_name_, source.name, format_exception_message(e));
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
