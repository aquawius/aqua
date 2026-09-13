#ifndef AQUA_RUNTIME_RUNTIME_CONFIG_H
#define AQUA_RUNTIME_RUNTIME_CONFIG_H

// runtime/CLI 层默认值与边界。统一放在 aqua::config，与 udp_config.h /
// grpc_config.h / buffer_config.h 共用同一命名空间，避免多个 config 命名空间
// 互相遮蔽。
//
// 分层：JB 相关的值一律**透传** buffer_config.h（Buffer 组件自己的默认），
// 本文件不复制第二份——同一个旋钮只在一处定义。

#include "aqua/audio/buffer/buffer_config.h"

#include <chrono>
#include <cstdint>

namespace aqua::config {

// gRPC 控制面默认端口（server 监听 / client 连接）。
inline constexpr std::uint16_t DEFAULT_RPC_PORT = 50051;
// UDP 数据面默认端口（server 监听；实际值由 server 通过 gRPC 下发给 client）。
inline constexpr std::uint16_t DEFAULT_UDP_PORT = 50000;
// server 默认绑定地址：监听所有 IPv4 接口。
inline constexpr char DEFAULT_BIND_IP[] = "0.0.0.0";
// 默认 client 显示名（仅用于 server 侧识别与日志）。
inline constexpr char DEFAULT_CLIENT_NAME[] = "aqua-client";

// client 抖动缓冲默认槽数（透传 Buffer 组件默认，= --jb-capacity 默认值）。
inline constexpr std::uint32_t DEFAULT_CLIENT_JB_CAPACITY_SLOTS = JB_DEFAULT_CAPACITY_SLOTS;
// server 采集→网络交接队列默认槽数（= --audio-queue-capacity 默认值）。
inline constexpr std::uint32_t DEFAULT_AUDIO_QUEUE_CAPACITY_SLOTS = 16;
// dispatcher 发包 pacing 的追赶深度（槽）：交接队列积压达到该深度时绕过 pacing
// 一次性清空（worker 被调度饿死后的追平路径）。必须明显大于自然摆动深度——
// capture 周期成串 2~3 包、叠加 worker 被调度延迟 1~2 包，队列深度在 0..5 间
// 摆动；阈值 4 实测（Wi-Fi，F=175/3.65ms）每秒触发 ~20 次 catchup 突发，pacing
// 名存实亡。8 槽 ≈ 29ms 积压才追平：自然摆动不会误入，真饿死也能快速恢复。
inline constexpr std::uint32_t DISPATCH_PACING_CATCHUP_DEPTH_SLOTS = 8;
// 显式指定 F（每包 sample frame 数）时的下限：再小则 RTP 头开销占比过高。
inline constexpr std::uint32_t MIN_FRAMES_PER_SLOT = 16;
// JB 容量合法区间（透传 Buffer 组件；下限是结构性的，上限纯护栏）。
inline constexpr std::uint32_t MIN_JB_CAPACITY_SLOTS = JB_MIN_CAPACITY_SLOTS;
inline constexpr std::uint32_t MAX_JB_CAPACITY_SLOTS = JB_MAX_CAPACITY_SLOTS;
inline constexpr std::uint32_t MAX_AUDIO_QUEUE_CAPACITY_SLOTS = 4096;
// 诊断快照与日志输出的节奏。
inline constexpr std::chrono::milliseconds DIAGNOSTICS_SNAPSHOT_INTERVAL { 1000 };

} // namespace aqua::config

#endif // AQUA_RUNTIME_RUNTIME_CONFIG_H
