//
// Created by QU on 25-2-11.
//

#ifndef AUDIO_PLAYBACK_H
#define AUDIO_PLAYBACK_H

#include <functional>
#include <span>
#include <memory>

#include <spdlog/spdlog.h>

class audio_playback {
public:
    // TODO: NEXT version, configured stream_config.
    struct stream_config {
        uint32_t rate { 48000 };
        uint32_t channels { 2 };
        uint32_t latency { 1024 };
    };

    // 工厂方法
    static std::unique_ptr<audio_playback> create();

    virtual ~audio_playback() = default;
    virtual bool init() = 0;
    virtual bool setup_stream() = 0;
    virtual bool start_playback() = 0;
    virtual bool stop_playback() = 0;
    [[nodiscard]] virtual bool is_playing() const = 0;
    [[nodiscard]] virtual const stream_config& get_format() const = 0;

    virtual bool push_packet_data(const std::vector<uint8_t>& origin_packet_data) = 0;

protected:
    stream_config m_stream_config;
};


#endif //AUDIO_PLAYBACK_H