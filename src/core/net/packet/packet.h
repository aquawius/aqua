#ifndef AQUA_PACKET_H
#define AQUA_PACKET_H

#include <cstdint>
#include <optional>
#include <span>

namespace aqua::net {

// UDP 包类型。所有整数按小端序读写（与 PCM 编码一致）。
enum class PacketType : std::uint8_t {
    Hello    = 1,
    HelloAck = 2,
    Audio    = 3,
};

// HELLO / HELLO_ACK 包：仅包含 session_id
#pragma pack(push, 1)
struct HelloPacket {
    PacketType type;        // 1 byte
    std::uint32_t session_id; // 4 bytes LE
};
static_assert(sizeof(HelloPacket) == 5);
#pragma pack(pop)

// AUDIO 包头，后面紧跟 PCM payload
// 注意：sample_position 为 uint32_t，48kHz/144帧/包下约 24.8 小时回绕。
// 接收方（DiagnosticsManager）应视为模运算值，回绕后诊断指标会跳变但不影响音频播放。
// 后续协议版本可扩展为 uint64_t。
#pragma pack(push, 1)
struct AudioPacketHeader {
    PacketType type;          // 1 byte
    std::uint32_t session_id; // 4 bytes LE
    std::uint32_t sequence;   // 4 bytes LE
    std::uint32_t sample_position; // 4 bytes LE（~24.8h 回绕 @48kHz）
    std::uint16_t payload_size;    // 2 bytes LE
};
static_assert(sizeof(AudioPacketHeader) == 15);
#pragma pack(pop)

// Audio 包的 session_id = 0 表示"广播到所有已连接 session"（而非单播到某个 session）。
// SessionManager 保证不生成 0（构造时 instance_id 强制非零），因此该哨兵值无歧义。
// 当前为单源广播模型，客户端忽略 Audio 包的 session_id；此常量供 server 侧语义使用。
inline constexpr std::uint32_t kBroadcastSessionId = 0;

// ---- 编码 ----

// 将 HelloPacket 编码到 out 缓冲。返回写入的字节数。
// out 必须至少 sizeof(HelloPacket) 字节。
std::size_t encode_hello(std::uint32_t session_id, std::span<std::byte> out) noexcept;

// 将 HelloAckPacket 编码到 out 缓冲。
std::size_t encode_hello_ack(std::uint32_t session_id, std::span<std::byte> out) noexcept;

// 将 AudioPacketHeader + payload 编码到 out。
// 返回写入的总字节数（header + payload），若 out 空间不足返回 0。
std::size_t encode_audio(std::uint32_t session_id,
                         std::uint32_t sequence,
                         std::uint32_t sample_position,
                         std::span<const std::byte> payload,
                         std::span<std::byte> out) noexcept;

// ---- 解码 ----

// 从原始字节解析包类型。
std::optional<PacketType> peek_type(std::span<const std::byte> in) noexcept;

// 解码 HELLO 包。校验类型和长度。
std::optional<HelloPacket> decode_hello(std::span<const std::byte> in) noexcept;

// 解码 AUDIO 包头。返回 header 和 payload 的 span（指向 in 内部）。
// 不拷贝 payload，零开销。
struct DecodedAudio {
    AudioPacketHeader header;
    std::span<const std::byte> payload;
};

std::optional<DecodedAudio> decode_audio(std::span<const std::byte> in) noexcept;

} // namespace aqua::net

#endif // AQUA_PACKET_H
