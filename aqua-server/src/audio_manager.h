//
// Created by QU on 25-2-10.
//

#ifndef AUDIO_MANAGER_H
#define AUDIO_MANAGER_H

#include <functional>
#include <span>
#include <memory>
#include <spdlog/spdlog.h>

class audio_manager
{
public:
    // 音频数据回调类型（数据所有权通过移动语义传递）
    using AudioDataCallback = std::function<void(std::span<const std::byte> audio_data)>;
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
    };

    // 工厂方法
    static std::unique_ptr<audio_manager> create();

    virtual ~audio_manager() = default;
    virtual bool init() = 0;
    virtual bool setup_stream() = 0;
    virtual bool start_capture(const AudioDataCallback& callback) = 0;
    virtual bool stop_capture() = 0;
    [[nodiscard]] virtual bool is_capturing() const = 0;
    [[nodiscard]] virtual AudioFormat get_preferred_format() const = 0;

    virtual void set_data_callback(AudioDataCallback callback) = 0;
    virtual void set_peak_callback(AudioPeakCallback callback) = 0;

protected:
    AudioFormat m_stream_config;
};

#endif // AUDIO_MANAGER_H
