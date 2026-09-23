#include "aqua/diagnostics/field_block.h"

#include <format>

namespace aqua::diagnostics {

std::string RateCounter::fmt(std::uint64_t total) const
{
    const auto now = std::chrono::steady_clock::now();
    std::uint64_t delta = 0;
    double rate = 0.0;
    if (initialized_) {
        delta = total >= last_ ? total - last_ : 0;
        const double elapsed = std::chrono::duration<double>(now - last_sample_).count();
        if (elapsed > 0.0) {
            rate = static_cast<double>(delta) / elapsed;
        }
    } else {
        initialized_ = true;
    }
    last_ = total;
    last_sample_ = now;
    // T/D/R：T=累计, D=距上次快照增量, R=每秒速率（一位小数）。
    return std::format("{}/{:.0f}/{:.1f}", total, static_cast<double>(delta), rate);
}

} // namespace aqua::diagnostics
