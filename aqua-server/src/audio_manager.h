//
// Created by QU on 25-2-10.
//

#ifndef AUDIO_MANAGER_H
#define AUDIO_MANAGER_H

#include "audio_format_common.hpp"

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

    // 使用公共命名空间中的类型
    using AudioEncoding = audio_common::AudioEncoding;
    using AudioFormat = audio_common::AudioFormat;

    // 工厂方法
    static std::shared_ptr<audio_manager> create();

    virtual ~audio_manager() = default;
    virtual bool init() = 0;

    // 你传入的格式有可能不是实际处理的格式，
    // 比如Windows的WASAPI共享模式不支持指定格式，在具体实现上就需要在内部实现转码的逻辑了
    virtual bool setup_stream(AudioFormat format) = 0;
    virtual bool start_capture(const AudioDataCallback& callback) = 0;
    virtual bool stop_capture() = 0;
    [[nodiscard]] virtual bool is_capturing() const = 0;

    // 实际正在处理的流格式，请以这个get_current_format为准
    [[nodiscard]] virtual AudioFormat get_current_format() const = 0;
    // 获取实现首选的默认格式
    [[nodiscard]] virtual AudioFormat get_preferred_format() const = 0;

    // 用于触发流格式更改逻辑
    virtual bool reconfigure_stream(const AudioFormat& new_format) = 0;

    virtual void set_data_callback(AudioDataCallback callback) = 0;
    virtual void set_peak_callback(AudioPeakCallback callback) = 0;

protected:
    AudioFormat m_stream_config { };
};

#endif // AUDIO_MANAGER_H
