#ifndef AQUA_RUNTIME_RUNTIME_CONFIG_H
#define AQUA_RUNTIME_RUNTIME_CONFIG_H

// runtime/CLI 层默认值与边界。统一放在 aqua::config，与 udp_config.h /
// grpc_config.h 共用同一命名空间，避免多个 config 命名空间互相遮蔽。
#include "aqua/audio/buffer/jitter_buffer.h"

#include <chrono>
#include <cstdint>

namespace aqua::config {

inline constexpr std::uint16_t DEFAULT_RPC_PORT = 50051;
inline constexpr std::uint16_t DEFAULT_UDP_PORT = 50000;
inline constexpr char DEFAULT_BIND_IP[] = "0.0.0.0";
inline constexpr char DEFAULT_CLIENT_NAME[] = "aqua-client";
inline constexpr std::uint32_t DEFAULT_CLIENT_JITTER_BUFFER_SLOTS = 30;
inline constexpr std::uint32_t DEFAULT_SERVER_NETWORK_QUEUE_SLOTS = 16;
inline constexpr std::uint32_t MIN_FRAMES_PER_SLOT = 16;
inline constexpr std::uint32_t MIN_JITTER_BUFFER_SLOTS = audio::JITTER_BUFFER_MIN_CAPACITY_SLOTS;
inline constexpr std::uint32_t MAX_JITTER_BUFFER_SLOTS = 4096;
inline constexpr std::uint32_t MAX_NETWORK_QUEUE_SLOTS = 4096;
inline constexpr std::chrono::milliseconds DIAGNOSTICS_SNAPSHOT_INTERVAL { 1000 };

} // namespace aqua::config

#endif // AQUA_RUNTIME_RUNTIME_CONFIG_H
