#ifndef AQUA_RUNTIME_RUNTIME_CONFIG_H
#define AQUA_RUNTIME_RUNTIME_CONFIG_H

#include <cstdint>

namespace aqua::runtime::config {

inline constexpr std::uint16_t DEFAULT_RPC_PORT = 50051;
inline constexpr std::uint16_t DEFAULT_UDP_PORT = 9999;
inline constexpr char DEFAULT_BIND_IP[] = "0.0.0.0";
inline constexpr std::uint32_t DEFAULT_CLIENT_JITTER_BUFFER_SLOTS = 30;
inline constexpr std::uint32_t DEFAULT_SERVER_NETWORK_QUEUE_SLOTS = 4;
inline constexpr std::uint32_t MIN_FRAMES_PER_SLOT = 16;
inline constexpr std::uint32_t MIN_JITTER_BUFFER_SLOTS = 4;
inline constexpr std::uint32_t MAX_JITTER_BUFFER_SLOTS = 4096;
inline constexpr std::uint32_t MAX_NETWORK_QUEUE_SLOTS = 4096;

} // namespace aqua::runtime::config

#endif // AQUA_RUNTIME_RUNTIME_CONFIG_H
