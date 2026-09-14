#ifndef AQUA_AUDIO_AUDIO_FORMAT_H
#define AQUA_AUDIO_AUDIO_FORMAT_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <utility>

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

// 编码 → 单个 sample 的字节数。索引即 std::to_underlying(AudioEncoding)，
// 顺序必须与枚举声明一致（static_assert 在下方锁定）。
inline constexpr std::array<std::uint8_t, 6> kBytesPerSample {
    0, // INVALID
    2, // PCM_S16LE
    4, // PCM_S32LE
    4, // PCM_F32LE
    3, // PCM_S24LE
    1, // PCM_U8
};
static_assert(kBytesPerSample[static_cast<std::size_t>(AudioEncoding::INVALID)] == 0);
static_assert(kBytesPerSample[static_cast<std::size_t>(AudioEncoding::PCM_S16LE)] == 2);
static_assert(kBytesPerSample[static_cast<std::size_t>(AudioEncoding::PCM_S32LE)] == 4);
static_assert(kBytesPerSample[static_cast<std::size_t>(AudioEncoding::PCM_F32LE)] == 4);
static_assert(kBytesPerSample[static_cast<std::size_t>(AudioEncoding::PCM_S24LE)] == 3);
static_assert(kBytesPerSample[static_cast<std::size_t>(AudioEncoding::PCM_U8)] == 1);

// 仅描述 PCM 数据本身，不包含 packet/frame policy、buffer latency 或设备信息。
// gRPC ConnectResponse 下发的格式会转换成这个类型，并作为当前音频流的权威格式。
struct AudioFormat {
    AudioEncoding encoding = AudioEncoding::INVALID;
    std::uint32_t channels = 0;
    std::uint32_t sample_rate = 0;

    [[nodiscard]] constexpr bool is_valid() const noexcept
    {
        const auto bytes = bytes_per_sample();
        return bytes > 0
            && channels > 0
            && channels <= AUDIO_FORMAT_MAX_CHANNELS
            && channels <= std::numeric_limits<std::uint32_t>::max() / bytes
            && sample_rate > 0
            && sample_rate <= AUDIO_FORMAT_MAX_SAMPLE_RATE;
    }

    // 单个 sample（单声道）的字节数。查表而非 switch：越界编码（可来自
    // 反序列化）返回 0 而不是 UB。
    [[nodiscard]] constexpr std::uint32_t bytes_per_sample() const noexcept
    {
        const auto idx = static_cast<std::size_t>(std::to_underlying(encoding));
        if (idx >= kBytesPerSample.size()) {
            return 0;
        }
        return static_cast<std::uint32_t>(kBytesPerSample[idx]);
    }

    // 一个 PCM sample frame（所有声道）的字节数。
    [[nodiscard]] constexpr std::uint32_t frame_bytes() const noexcept
    {
        const auto bytes = bytes_per_sample();
        if (bytes == 0 || channels > std::numeric_limits<std::uint32_t>::max() / bytes) {
            return 0;
        }
        return bytes * channels;
    }

    // frame_count 个 audio frame 所需的字节数；溢出时返回 0。
    [[nodiscard]] constexpr std::size_t bytes_for_frames(std::size_t frame_count) const noexcept
    {
        const auto bytes = frame_bytes();
        if (bytes == 0 || frame_count > std::numeric_limits<std::size_t>::max() / bytes) {
            return 0;
        }
        return frame_count * bytes;
    }

    // data_size 是否恰好由整数个 PCM sample frame 构成。
    [[nodiscard]] constexpr bool is_frame_aligned(std::size_t data_size) const noexcept
    {
        const auto bytes = frame_bytes();
        return bytes != 0 && data_size % bytes == 0;
    }

    // 从字节数反推出 sample-frame 数量；不是完整 frame 或格式非法时返回 nullopt。
    // 0 字节是合法输入，对应 0 frames。
    [[nodiscard]] constexpr std::optional<std::size_t> frames_from_bytes(std::size_t data_size) const noexcept
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
    [[nodiscard]] constexpr std::byte silence_byte() const noexcept
    {
        return encoding == AudioEncoding::PCM_U8 ? std::byte { 0x80 } : std::byte { 0x00 };
    }

    // frame_count 个 sample frame 的时长（ms）。JB 每槽粒度 = 包时长，
    // 诊断与 auto-F 的公共口径——此前 server_runtime / client_runtime /
    // target_controller 各内联一份同样的算式。
    [[nodiscard]] constexpr double duration_ms(std::uint32_t frame_count) const noexcept
    {
        if (sample_rate == 0) {
            return 0.0;
        }
        return static_cast<double>(frame_count) * 1000.0 / static_cast<double>(sample_rate);
    }

    [[nodiscard]] constexpr bool operator==(const AudioFormat&) const noexcept = default;
};

// 由字节预算反推能容纳的 sample frame 数（向下取整）。
// 常用于由 MTU payload 预算推导每 AudioFrame 的 sample frame 数 F。
// format 非法、frame_bytes 为 0 或预算不足一帧 → 0。
[[nodiscard]] constexpr std::uint32_t frame_count_for_budget(const AudioFormat& format,
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
[[nodiscard]] constexpr std::uint32_t frame_count_for_duration(const AudioFormat& format,
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

// ---- 编译期自测：MTU 预算与时长上限的关键档位 ----
static_assert(!AudioFormat { }.is_valid());
static_assert(!AudioFormat { AudioEncoding::PCM_S16LE, 0, 48000 }.is_valid());
static_assert(AudioFormat { AudioEncoding::PCM_S16LE, 2, 48000 }.is_valid());
static_assert(AudioFormat { AudioEncoding::PCM_S16LE, 2, 48000 }.frame_bytes() == 4);
static_assert(AudioFormat { AudioEncoding::PCM_F32LE, 2, 48000 }.frame_bytes() == 8);
static_assert(frame_count_for_budget(AudioFormat { AudioEncoding::PCM_F32LE, 2, 48000 }, 1400) == 175);
static_assert(frame_count_for_budget(AudioFormat { AudioEncoding::PCM_F32LE, 2, 48000 }, 3) == 0);
static_assert(frame_count_for_duration(AudioFormat { AudioEncoding::PCM_F32LE, 2, 48000 }, 5.0) == 240);
static_assert(AudioFormat { AudioEncoding::PCM_U8, 1, 48000 }.silence_byte() == std::byte { 0x80 });
static_assert(AudioFormat { AudioEncoding::PCM_S16LE, 1, 48000 }.silence_byte() == std::byte { 0x00 });

} // namespace aqua::audio

#endif // AQUA_AUDIO_AUDIO_FORMAT_H
