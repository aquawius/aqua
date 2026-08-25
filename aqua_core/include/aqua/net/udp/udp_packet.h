#ifndef AQUA_NET_UDP_PACKET_H
#define AQUA_NET_UDP_PACKET_H

// Aqua UDP wire-level 协议：数据报的编码/解码。
//
// 每个 AudioFrame 恰好装入一个 UDP datagram（server 按 ~MTU 打包固定大小，
// 见 doc/buffer_design.md §15）。datagram 布局（little-endian，所有支持平台
// x86/x64/ARM/ARM64 均为 LE）：
//
//   [0]         type                (1B)  PacketType
//   Audio:      [1..8]  sequence    (u64)  音频层单调序号
//               [9..]   pcm payload        完整 AudioFrame 的 PCM（F × frame_bytes）
//   Hello/Ack:  [1..4]  session_id  (u32)  Connect 下发的 session id
//
// `F`（每 AudioFrame 的 sample frame 数）由控制面下发、不进包；`timestamp_ns`
// 为保留字段、不进包（JitterBuffer 不使用）。

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace aqua::net {

enum class PacketType : std::uint8_t {
    Invalid = 0,
    Hello = 1,
    HelloAck = 2,
    Audio = 3,
};

inline constexpr std::size_t kPacketTypeBytes = 1;
inline constexpr std::size_t kAudioHeaderBytes = 1 + sizeof(std::uint64_t); // 9
inline constexpr std::size_t kAudioSequenceOffset = 1;
inline constexpr std::size_t kAudioPayloadOffset = kAudioHeaderBytes;
inline constexpr std::size_t kHelloPacketBytes = 1 + sizeof(std::uint32_t); // 5
inline constexpr std::size_t kHelloSessionIdOffset = 1;

// 编码：构造完整 datagram（拷贝语义）。
std::vector<std::byte> encode_audio_packet(std::uint64_t sequence, std::span<const std::byte> pcm);
std::vector<std::byte> encode_hello_packet(std::uint32_t session_id);
std::vector<std::byte> encode_hello_ack_packet(std::uint32_t session_id);

// 读取 datagram 的 type；长度不足或未知值 → Invalid。
PacketType decode_packet_type(std::span<const std::byte> packet) noexcept;

// 从 Audio 包解析 sequence 与 payload。payload 为指向输入 datagram 的视图（不拷贝）；
// 长度不足以容纳 header 时返回 false。
bool decode_audio_packet(std::span<const std::byte> packet,
    std::uint64_t& sequence, std::span<const std::byte>& pcm) noexcept;

// 从 Hello/HelloAck 包解析 session_id；长度不足返回 false。
bool decode_hello_packet(std::span<const std::byte> packet, std::uint32_t& session_id) noexcept;

} // namespace aqua::net

#endif // AQUA_NET_UDP_PACKET_H
