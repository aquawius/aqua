#ifndef AQUA_NET_UDP_NETWORK_FRAME_H
#define AQUA_NET_UDP_NETWORK_FRAME_H

// NetworkFrame：UDP 数据面统一的 wire 帧（Audio / Heartbeat / HeartbeatAck）。
//
// client→server 只有一种包：Heartbeat（association 建立 + NAT/endpoint 续命，
// 同一包型；server 按 session 状态区分：Created→建立并回 ACK，Connected→只刷新）。
// 建连是 server 端状态机的一次性跃迁，不是第二种包类型。
//
// 三层类型边界（audio 域 → net 域）：
//   AudioBlock   (audio 域，capture 产出)    —— 变长纯 PCM 块
//   AudioFrame   (audio 域，packetizer 产出) —— 定长帧（sequence + frame_count + data）
//   NetworkFrame (net 域)                   —— AudioFrame 打上网络包头后的 wire 帧
//
// Audio wire 布局（RTP 12-byte header，大端序，RFC 3550 §5.1）：
//   [0]         V=2, P/X/CC=0               (0x80)
//   [1]         M=0, PT                     (PT=96 PCM-LE；M 恒 0，置位即非法)
//   [2..3]      sequence           (u16 BE) 包排序/乱序/丢包检测专用
//   [4..7]      timestamp          (u32 BE) 媒体时钟，单位=会话采样率
//   [8..11]     SSRC               (u32 BE) 发送流随机 ID
//   [12..]     payload                      完整 AudioFrame 的 PCM（F × frame_bytes）
// Heartbeat/HeartbeatAck wire 布局（5-byte，小端；控制面包沿用既有 convention）：
//   [0]         type              (1B) PacketType
//   [1..4]      session_id (u32 LE) Connect 下发的 session id
//
// 说明：
// - wire sequence 是 16-bit（回绕由接收端按 RFC 3550 附录 A 展开成 u64 extended
//   sequence；JB 内部一律 u64，不感知回绕）；timestamp 是 media timeline，
//   sequence 只做 ordering——二者职责分离，timestamp 不做单调/连续性判定。
// - SSRC 由 server 每 run 随机生成；client 钉住首包 SSRC（learned_peer_endpoint 同模型），
//   不等即丢（计入 malformed）。
// - `F`（每 AudioFrame 的 sample frame 数）由控制面下发、不进包。
// - M 恒 0：不断流语义，不用 M 位做任何带外信令。

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace aqua::net {

enum class PacketType : std::uint8_t {
    Invalid = 0,
    Heartbeat = 1,
    HeartbeatAck = 2,
    Audio = 3,
};

// RTP 音频负载类型（动态区）：96 = PCM-LE 裸流；97 预留给 Opus。
inline constexpr std::uint8_t kRtpPayloadTypePcm = 96;

inline constexpr std::size_t kPacketTypeBytes = 1;
inline constexpr std::size_t kRtpHeaderBytes = 12;
inline constexpr std::size_t kRtpSequenceOffset = 2;
inline constexpr std::size_t kRtpTimestampOffset = 4;
inline constexpr std::size_t kRtpSsrcOffset = 8;
inline constexpr std::size_t kRtpPayloadOffset = kRtpHeaderBytes;
inline constexpr std::size_t kHeartbeatPacketBytes = 1 + sizeof(std::uint32_t); // 5
inline constexpr std::size_t kHeartbeatSessionIdOffset = 1;

// wire 16-bit sequence → u64 extended sequence（RFC 3550 附录 A）。
// last_ext 无值 = 首包，直接采用 raw；否则按 0x8000 半窗判定回绕/乱序。
// 纯函数，收包 strand 外也可独立单测。
[[nodiscard]] inline std::uint64_t extend_rtp_sequence(
    std::optional<std::uint64_t> last_ext, std::uint16_t raw) noexcept
{
    if (!last_ext.has_value()) {
        return raw;
    }
    const std::uint64_t base = (*last_ext & ~std::uint64_t { 0xFFFF }) | raw;
    const std::uint16_t last_raw = static_cast<std::uint16_t>(*last_ext & 0xFFFF);
    if (raw < last_raw && static_cast<std::uint16_t>(last_raw - raw) > 0x8000) {
        return base + 0x10000; // 前向回绕
    }
    if (raw > last_raw && static_cast<std::uint16_t>(raw - last_raw) > 0x8000 && base >= 0x10000) {
        return base - 0x10000; // 回绕边界附近的迟到乱序
    }
    return base;
}

// UDP 数据面的一帧（值语义）。payload 是非拥有视图：
//   - 工厂构造时指向调用方传入的 AudioFrame.data，仅在 encode() 返回前有效；
//   - decode() 后指向传入 wire 缓冲内部，仅在 wire 存活期间有效。
class NetworkFrame {
public:
    // 工厂：构造各类型帧。
    [[nodiscard]] static NetworkFrame audio(std::uint16_t sequence, std::uint32_t timestamp,
        std::uint32_t ssrc, std::span<const std::byte> payload);
    [[nodiscard]] static NetworkFrame heartbeat(std::uint32_t session_id);
    [[nodiscard]] static NetworkFrame heartbeat_ack(std::uint32_t session_id);

    // 编码为完整 wire datagram（拷贝 payload）。Invalid 帧返回空向量。
    [[nodiscard]] std::vector<std::byte> encode() const;

    // 从 wire datagram 解码；长度不足 / 版本-PT-M 非法返回 std::nullopt。
    [[nodiscard]] static std::optional<NetworkFrame> decode(std::span<const std::byte> wire) noexcept;

    [[nodiscard]] PacketType type() const noexcept { return type_; }
    // Audio 帧有效：RTP 包序号 / 时间戳 / SSRC / PT。
    [[nodiscard]] std::uint16_t rtp_sequence() const noexcept { return rtp_sequence_; }
    [[nodiscard]] std::uint32_t timestamp() const noexcept { return timestamp_; }
    [[nodiscard]] std::uint32_t ssrc() const noexcept { return ssrc_; }
    [[nodiscard]] std::uint8_t payload_type() const noexcept { return payload_type_; }
    // Heartbeat/HeartbeatAck 帧有效：session id。
    [[nodiscard]] std::uint32_t session_id() const noexcept { return session_id_; }
    // Audio 帧有效：PCM payload（非拥有视图）。
    [[nodiscard]] std::span<const std::byte> payload() const noexcept { return payload_; }

private:
    NetworkFrame() = default;

    PacketType type_ = PacketType::Invalid;
    std::uint16_t rtp_sequence_ = 0;
    std::uint32_t timestamp_ = 0;
    std::uint32_t ssrc_ = 0;
    std::uint8_t payload_type_ = 0;
    std::uint32_t session_id_ = 0;
    std::span<const std::byte> payload_;
};

} // namespace aqua::net

#endif // AQUA_NET_UDP_NETWORK_FRAME_H
