//
// Created by QU on 25-2-11.
//

#ifndef AUDIO_PLAYBACK_H
#define AUDIO_PLAYBACK_H

#include <functional>
#include <span>
#include <memory>
#include <vector>
#include <cstddef>

#include <spdlog/spdlog.h>

class audio_playback
{
public:
    using AudioDataCallback = std::function<void(std::span<const float> audio_data)>;
    using AudioPeakCallback = std::function<void(float)>;

    enum class AudioEncoding
    {
        INVALID = 0,
        PCM_S16LE = 1,
        PCM_S32LE = 2,
        PCM_F32LE = 3,
        PCM_S24LE = 4,
        PCM_U8 = 5
    };

    struct AudioFormat
    {
        AudioEncoding encoding;
        uint32_t channels;
        uint32_t sample_rate;
        uint32_t bit_depth;

        bool operator==(const AudioFormat& other) const
        {
            return encoding == other.encoding &&
                channels == other.channels &&
                sample_rate == other.sample_rate &&
                bit_depth == other.bit_depth;
        }

        bool operator!=(const AudioFormat& other) const
        {
            return !(*this == other);
        }
    };

    // 工厂方法
    static std::unique_ptr<audio_playback> create();

    virtual ~audio_playback() = default;
    virtual bool init() = 0;
    virtual bool setup_stream() = 0;
    virtual bool start_playback() = 0;
    virtual bool stop_playback() = 0;
    [[nodiscard]] virtual bool is_playing() const = 0;

    virtual void set_format(AudioFormat) = 0;
    [[nodiscard]] virtual AudioFormat get_current_format() const = 0;

    virtual bool push_packet_data(std::span<const std::byte> packet_data) = 0;
    virtual void set_peak_callback(AudioPeakCallback callback) = 0;

protected:
    AudioFormat m_stream_config = { };
};

#endif //AUDIO_PLAYBACK_H
