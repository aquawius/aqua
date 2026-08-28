#ifndef AQUA_CLI_PARSER_COMMON_H
#define AQUA_CLI_PARSER_COMMON_H

// CLI parser 共用：音频参数解析、F 推导、解析结果枚举。

#include "aqua/audio/audio_format.h"
#include "aqua/logger/logger.h"
#include "aqua/net/udp/udp_config.h"
#include "aqua/audio/packetizer/audio_packetizer.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace aqua::cli {

// 解析结果：Run = 成功（config 已填充）；Help = 已打印 usage，应退出(0)；Error = 参数错误，应退出(1)。
enum class ParseOutcome {
    Run,
    Help,
    Error,
};

// encoding 字符串 → AudioEncoding；无法识别返回 nullopt。
inline std::optional<audio::AudioEncoding> parse_encoding(std::string_view name)
{
    if (name == "s16") {
        return audio::AudioEncoding::PCM_S16LE;
    }
    if (name == "s24") {
        return audio::AudioEncoding::PCM_S24LE;
    }
    if (name == "s32") {
        return audio::AudioEncoding::PCM_S32LE;
    }
    if (name == "f32") {
        return audio::AudioEncoding::PCM_F32LE;
    }
    if (name == "u8") {
        return audio::AudioEncoding::PCM_U8;
    }
    return std::nullopt;
}

inline audio::AudioFormat make_format(audio::AudioEncoding enc, std::uint32_t channels,
    std::uint32_t sample_rate)
{
    audio::AudioFormat fmt;
    fmt.encoding = enc;
    fmt.channels = channels;
    fmt.sample_rate = sample_rate;
    return fmt;
}

// MTU 净荷预算：按 IPv6-safe 计算（IPv6 头 40 字节，比 IPv4 的 20 更大）。
//   1500 − 40(IPv6) − 8(UDP) − 9(wire 头，见 network_frame.h kAudioHeaderBytes) = 1443。
inline constexpr std::size_t kMtuPayloadBudget = config::UDP_AUDIO_PAYLOAD_BYTES;

// F 确定：显式指定则用指定值（并校验 ≤ MTU 预算）；否则按 MTU 预算反推。
// 返回 0 表示非法（显式 F 超 MTU 预算 / 溢出，或自动推导失败）。
inline std::uint32_t resolve_frame_count(std::uint32_t explicit_fps,
    const audio::AudioFormat& fmt)
{
    if (explicit_fps != 0) {
        // 显式 F 换算成字节数（bytes_for_frames 溢出返回 0），必须 ≤ MTU 预算，
        // 否则一个 AudioFrame 会超过单个 UDP 包容量导致 IP 分片（实时音频不可接受）。
        const auto bytes = fmt.bytes_for_frames(explicit_fps);
        if (bytes == 0 || bytes > kMtuPayloadBudget) {
            return 0;
        }
        return explicit_fps;
    }
    return audio::frame_count_for_budget(fmt, kMtuPayloadBudget);
}

} // namespace aqua::cli

#endif // AQUA_CLI_PARSER_COMMON_H
