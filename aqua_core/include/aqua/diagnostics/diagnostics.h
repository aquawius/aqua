#ifndef AQUA_DIAGNOSTICS_DIAGNOSTICS_H
#define AQUA_DIAGNOSTICS_DIAGNOSTICS_H

// Debug-only diagnostic snapshot builder.
// Diagnostics never owns runtime state; registered sources are sampled on demand.
// Output is emitted as one compact line at Debug level and is skipped entirely
// when Debug logging is disabled.

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace aqua::diagnostics {

class Diagnostics {
public:
    using SourceFn = std::function<std::string()>;

    explicit Diagnostics(std::string component_name);

    Diagnostics(const Diagnostics&) = delete;
    Diagnostics& operator=(const Diagnostics&) = delete;

    void add_source(std::string name, SourceFn fn);

    // Registers a monotonically increasing counter. The generated section contains
    // total=, delta= and rate=/s fields. Rate is calculated against the actual
    // elapsed time between snapshots, so scheduler jitter does not distort it.
    using CounterFn = std::function<std::uint64_t()>;
    void add_counter(std::string name, CounterFn fn);

    // Emits one line such as:
    //   Client diag: state=running net{...} jb{...} playback{...}
    // Returns immediately without evaluating sources when Debug is disabled.
    void log_debug() const;

private:
    struct Source {
        std::string name;
        SourceFn fn;
    };

    std::string component_name_;
    std::vector<Source> sources_;
};

} // namespace aqua::diagnostics

#endif // AQUA_DIAGNOSTICS_DIAGNOSTICS_H
