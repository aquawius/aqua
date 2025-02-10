//
// Created by QU on 25-2-10.
//

#ifndef AUDIO_MANAGER_H
#define AUDIO_MANAGER_H

#include <functional>
#include <span>
#include <memory>
#include <spdlog/spdlog.h>

class audio_manager {
public:
    // 音频数据回调类型（数据所有权通过移动语义传递）
    using AudioDataCallback = std::function<void(std::span<const float> audio_data)>;

    // TODO: NEXT version, configured stream_config.
    struct stream_config {
        uint32_t rate { 48000 };
        uint32_t channels { 2 };
        uint32_t latency { 1024 };
    };

    // 工厂方法
    static std::unique_ptr<audio_manager> create();

    virtual ~audio_manager() = default;
    virtual bool init() = 0;
    virtual bool setup_stream() = 0;
    virtual bool start_capture(const AudioDataCallback& callback) = 0;
    virtual bool stop_capture() = 0;
    [[nodiscard]] virtual bool is_capturing() const = 0;
    [[nodiscard]] virtual const stream_config& get_format() const = 0;

protected:
    stream_config m_stream_config;
};

#endif // AUDIO_MANAGER_H