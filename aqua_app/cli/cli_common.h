#ifndef AQUA_CLI_COMMON_H
#define AQUA_CLI_COMMON_H

// CLI 验证工具共用的小工具：音频参数解析与 F 推导（server / client 共用）。

#include "aqua/audio/audio_format.h"
#include "aqua/audio/packetizer/audio_packetizer.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace aqua::cli {

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

// MTU 净荷预算：1500 − 20(IP) − 8(UDP) − 9(wire 头)，见 udp_packet.h kAudioHeaderBytes。
inline constexpr std::size_t kMtuPayloadBudget = 1500 - 20 - 8 - 9;

// F 确定：显式指定则用指定值；否则按 MTU 预算反推。
inline std::uint32_t resolve_frames_per_slot(std::uint32_t explicit_fps,
    const audio::AudioFormat& fmt)
{
    if (explicit_fps != 0) {
        return explicit_fps;
    }
    return audio::frame_count_for_budget(fmt, kMtuPayloadBudget);
}

} // namespace aqua::cli

#endif // AQUA_CLI_COMMON_H
