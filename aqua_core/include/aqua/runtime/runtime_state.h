#ifndef AQUA_RUNTIME_RUNTIME_STATE_H
#define AQUA_RUNTIME_RUNTIME_STATE_H

#include <cstdint>
#include <string_view>

namespace aqua::runtime {

// Lifecycle is controlled by the runtime owner. Audio backend event threads may
// publish a transition to Degraded, but must never perform teardown themselves.
enum class RuntimeState : std::uint8_t {
    Created = 0,
    Starting,
    Running,
    Degraded,
    Stopping,
    Stopped,
};

constexpr std::string_view runtime_state_name(RuntimeState state) noexcept
{
    switch (state) {
    case RuntimeState::Created: return "created";
    case RuntimeState::Starting: return "starting";
    case RuntimeState::Running: return "running";
    case RuntimeState::Degraded: return "degraded";
    case RuntimeState::Stopping: return "stopping";
    case RuntimeState::Stopped: return "stopped";
    }
    return "unknown";
}

} // namespace aqua::runtime

#endif // AQUA_RUNTIME_RUNTIME_STATE_H
