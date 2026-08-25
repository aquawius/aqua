#include "aqua/diagnostics/diagnostics.h"

#include "aqua/logger/logger.h"

#include <utility>

namespace aqua::diagnostics {

void Diagnostics::add_source(std::string name, SourceFn fn)
{
    sources_.push_back(Source { std::move(name), std::move(fn) });
}

void Diagnostics::print() const
{
    for (const auto& source : sources_) {
        try {
            const std::string line = source.fn();
            aqua::log_info_fmt("[{}] {}", source.name, line);
        } catch (...) {
            // 诊断源异常不影响主流程。
        }
    }
}

} // namespace aqua::diagnostics
