#ifndef AQUA_NET_UDP_NETWORK_FRAME_H
#define AQUA_NET_UDP_NETWORK_FRAME_H

// NetworkFrame：UDP 数据面统一的 wire 帧（Audio / Hello / HelloAck 皆属此类）。
//
// 三层类型边界（audio 域 → net 域）：
//   AudioBlock   (audio 域，capture 产出)    —— 变长纯 PCM 块
//   AudioFrame   (audio 域，packetizer 产出) —— 定长帧（sequence + frame_count + data）
//   NetworkFrame (net 域)                   —— AudioFrame 打上网络包头后的 wire 帧
//
// wire 布局（little-endian，所有支持平台 x86/x64/ARM/ARM64 均为 LE）：
//   [0]         type              (1B) PacketType
//   Audio:      [1..8]  sequence  (u64)  音频层单调序号
//               [9..]   payload          完整 AudioFrame 的 PCM（F × frame_bytes）
//   Hello/Ack:  [1..4]  session_id (u32) Connect 下发的 session id
//
// `F`（每 AudioFrame 的 sample frame 数）由控制面下发、不进包。

#include <cstddef>
#include <cstdint>
#include <optional>
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

// UDP 数据面的一帧（值语义）。payload 是非拥有视图：
//   - 工厂构造时指向调用方传入的 AudioFrame.data，仅在 encode() 返回前有效；
//   - decode() 后指向传入 wire 缓冲内部，仅在 wire 存活期间有效。
class NetworkFrame {
public:
    // 工厂：构造各类型帧。
    [[nodiscard]] static NetworkFrame audio(std::uint64_t sequence,
        std::span<const std::byte> payload);
    [[nodiscard]] static NetworkFrame hello(std::uint32_t session_id);
    [[nodiscard]] static NetworkFrame hello_ack(std::uint32_t session_id);

    // 编码为完整 wire datagram（拷贝 payload）。Invalid 帧返回空向量。
    [[nodiscard]] std::vector<std::byte> encode() const;

    // 从 wire datagram 解码；长度不足 / 未知类型返回 std::nullopt。
    [[nodiscard]] static std::optional<NetworkFrame> decode(std::span<const std::byte> wire) noexcept;

    [[nodiscard]] PacketType type() const noexcept { return type_; }
    // Audio 帧有效：音频层单调 sequence。
    [[nodiscard]] std::uint64_t sequence() const noexcept { return sequence_; }
    // Hello/HelloAck 帧有效：session id。
    [[nodiscard]] std::uint32_t session_id() const noexcept { return session_id_; }
    // Audio 帧有效：PCM payload（非拥有视图）。
    [[nodiscard]] std::span<const std::byte> payload() const noexcept { return payload_; }

private:
    NetworkFrame() = default;

    PacketType type_ = PacketType::Invalid;
    std::uint64_t sequence_ = 0;
    std::uint32_t session_id_ = 0;
    std::span<const std::byte> payload_;
};

} // namespace aqua::net

#endif // AQUA_NET_UDP_NETWORK_FRAME_H
