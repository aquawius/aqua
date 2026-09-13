#ifndef AQUA_AUDIO_AUDIO_FORMAT_H
#define AQUA_AUDIO_AUDIO_FORMAT_H

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>

namespace aqua::audio {

inline constexpr std::uint32_t AUDIO_FORMAT_MAX_CHANNELS = 64;
inline constexpr std::uint32_t AUDIO_FORMAT_MAX_SAMPLE_RATE = 768000;

// 音频编码格式，与 aqua_service.proto 中 AudioFormat.Encoding 一一对应。
// 枚举值必须保持同步，禁止随意修改。
enum class AudioEncoding : std::uint8_t {
    INVALID = 0,
    PCM_S16LE = 1,
    PCM_S32LE = 2,
    PCM_F32LE = 3,
    PCM_S24LE = 4,
    PCM_U8 = 5,
};

// 仅描述 PCM 数据本身，不包含 packet/frame policy、buffer latency 或设备信息。
// gRPC ConnectResponse 下发的格式会转换成这个类型，并作为当前音频流的权威格式。
struct AudioFormat {
    AudioEncoding encoding = AudioEncoding::INVALID;
    std::uint32_t channels = 0;
    std::uint32_t sample_rate = 0;

    [[nodiscard]] bool is_valid() const noexcept
    {
        const auto bytes = bytes_per_sample();
        return bytes > 0
            && channels > 0
            && channels <= AUDIO_FORMAT_MAX_CHANNELS
            && channels <= std::numeric_limits<std::uint32_t>::max() / bytes
            && sample_rate > 0
            && sample_rate <= AUDIO_FORMAT_MAX_SAMPLE_RATE;
    }

    // 单个 sample（单声道）的字节数。
    [[nodiscard]] std::uint32_t bytes_per_sample() const noexcept
    {
        switch (encoding) {
        case AudioEncoding::PCM_S16LE:
            return 2;
        case AudioEncoding::PCM_S24LE:
            return 3;
        case AudioEncoding::PCM_S32LE:
            return 4;
        case AudioEncoding::PCM_F32LE:
            return 4;
        case AudioEncoding::PCM_U8:
            return 1;
        case AudioEncoding::INVALID:
            return 0;
        }
        return 0;
    }

    // 一个 PCM sample frame（所有声道）的字节数。
    [[nodiscard]] std::uint32_t frame_bytes() const noexcept
    {
        const auto bytes = bytes_per_sample();
        if (bytes == 0 || channels > std::numeric_limits<std::uint32_t>::max() / bytes) {
            return 0;
        }
        return bytes * channels;
    }

    // frame_count 个 audio frame 所需的字节数；溢出时返回 0。
    [[nodiscard]] std::size_t bytes_for_frames(std::size_t frame_count) const noexcept
    {
        const auto bytes = frame_bytes();
        if (bytes == 0 || frame_count > std::numeric_limits<std::size_t>::max() / bytes) {
            return 0;
        }
        return frame_count * bytes;
    }

    // data_size 是否恰好由整数个 PCM sample frame 构成。
    [[nodiscard]] bool is_frame_aligned(std::size_t data_size) const noexcept
    {
        const auto bytes = frame_bytes();
        return bytes != 0 && data_size % bytes == 0;
    }

    // 从字节数反推出 sample-frame 数量；不是完整 frame 或格式非法时返回 nullopt。
    // 0 字节是合法输入，对应 0 frames。
    [[nodiscard]] std::optional<std::size_t> frames_from_bytes(std::size_t data_size) const noexcept
    {
        if (!is_valid()) {
            return std::nullopt;
        }
        const auto bytes = frame_bytes();
        if (bytes == 0 || data_size % bytes != 0) {
            return std::nullopt;
        }
        return data_size / bytes;
    }

    // 该编码的数字静音字节：PCM_U8 为 0x80（无符号中点），其余为 0x00。
    // 所有补静音路径（JitterBuffer 缺帧、后端欠填、capture 合成静音）必须用它，
    // 不得统一填 0。
    [[nodiscard]] std::byte silence_byte() const noexcept
    {
        return encoding == AudioEncoding::PCM_U8 ? std::byte { 0x80 } : std::byte { 0x00 };
    }

    [[nodiscard]] bool operator==(const AudioFormat&) const noexcept = default;
};

// 由字节预算反推能容纳的 sample frame 数（向下取整）。
// 常用于由 MTU payload 预算推导每 AudioFrame 的 sample frame 数 F。
// format 非法、frame_bytes 为 0 或预算不足一帧 → 0。
inline std::uint32_t frame_count_for_budget(const AudioFormat& format,
    std::size_t payload_budget_bytes) noexcept
{
    if (!format.is_valid()) {
        return 0;
    }
    const std::size_t frame_bytes = format.frame_bytes();
    if (frame_bytes == 0 || payload_budget_bytes < frame_bytes) {
        return 0;
    }
    const std::size_t count = payload_budget_bytes / frame_bytes;
    if (count > std::numeric_limits<std::uint32_t>::max()) {
        return 0;
    }
    return static_cast<std::uint32_t>(count);
}

// 由包时长上限反推能容纳的 sample frame 数（向下取整）。
// 与 frame_count_for_budget 配合做 auto-F：F = min(MTU 预算, 时长上限)，
// 使不同格式的包时长处于同一量级（JB 每槽粒度 = 包时长，低延迟链路的关键）。
// format 非法、max_duration_ms <= 0 或不足一帧 → 0。
inline std::uint32_t frame_count_for_duration(const AudioFormat& format,
    double max_duration_ms) noexcept
{
    if (!format.is_valid() || max_duration_ms <= 0.0) {
        return 0;
    }
    const double count
        = static_cast<double>(format.sample_rate) * max_duration_ms / 1000.0;
    if (count < 1.0
        || count > static_cast<double>(std::numeric_limits<std::uint32_t>::max())) {
        return 0;
    }
    return static_cast<std::uint32_t>(count);
}

} // namespace aqua::audio

#endif // AQUA_AUDIO_AUDIO_FORMAT_H
