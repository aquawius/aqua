#ifndef AQUA_RUNTIME_RUNTIME_STATE_H
#define AQUA_RUNTIME_RUNTIME_STATE_H

#include <chrono>
#include <cstdint>

namespace aqua::runtime {

inline constexpr std::chrono::milliseconds RUNTIME_CONTROL_POLL_INTERVAL { 500 };

enum class RuntimeState : std::uint8_t {
    Created,
    Starting,
    Running,
    Degraded,
    Stopping,
    Stopped,
};

inline constexpr const char* runtime_state_name(RuntimeState state) noexcept
{
    switch (state) {
    case RuntimeState::Created:
        return "created";
    case RuntimeState::Starting:
        return "starting";
    case RuntimeState::Running:
        return "running";
    case RuntimeState::Degraded:
        return "degraded";
    case RuntimeState::Stopping:
        return "stopping";
    case RuntimeState::Stopped:
        return "stopped";
    default:
        return "unknown";
    }
}

} // namespace aqua::runtime

#endif // AQUA_RUNTIME_RUNTIME_STATE_H
