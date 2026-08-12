#ifndef AQUA_AUDIO_BACKEND_H
#define AQUA_AUDIO_BACKEND_H

#include "core/public/audio_format.h"

#include <cstddef>
#include <functional>
#include <memory>
#include <span>

namespace aqua::audio {

// 音频采集后端抽象接口。
// 回调在音频实时线程触发，遵守 §10/§15.2 约束（无锁、无分配、无阻塞）。
// 回调内只做 RingBuffer 写入，不直接调用 UDP / SessionManager。
class CaptureBackend {
public:
    using CaptureCallback = std::function<void(std::span<const std::byte> pcm)>;

    virtual ~CaptureBackend() = default;

    // 启动采集。成功返回 true，并输出实际使用的 AudioFormat（WASAPI 使用设备 mix format）。
    virtual bool start(CaptureCallback cb, AudioFormat& out_format) = 0;
    virtual void stop() = 0;
};

// 音频播放后端抽象接口。
// FillCallback 由播放线程调用，填充 out 缓冲。返回实际填充字节数；不足部分播放静音。
class PlaybackBackend {
public:
    using FillCallback = std::function<std::size_t(std::span<std::byte> out)>;

    virtual ~PlaybackBackend() = default;

    // 启动播放。成功返回 true，并输出实际使用的 AudioFormat。
    virtual bool start(AudioFormat format, FillCallback cb) = 0;
    virtual void stop() = 0;
};

// 工厂：平台相关，根据编译期宏选择实现。
std::unique_ptr<CaptureBackend> create_capture_backend();
std::unique_ptr<PlaybackBackend> create_playback_backend();

} // namespace aqua::audio

#endif // AQUA_AUDIO_BACKEND_H
