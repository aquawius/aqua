#ifndef AQUA_AAUDIO_PLAYBACK_H
#define AQUA_AAUDIO_PLAYBACK_H

#include "core/audio/backend/audio_backend_factory.h"
#include "core/public/audio_format.h"

// 仅 Android 编译：本头文件由 audio_backend_factory.cpp 在 __ANDROID__ 下包含。
#include <aaudio/AAudio.h>

#include <atomic>
#include <cstdint>

namespace aqua::audio {

// Android AAudio 播放后端（输出流 + data callback）。
//
// 线程模型：
//   - start()/stop() 在调用方线程（client_runtime 会话线程）。
//   - data callback / error callback 在 AAudio 内部实时线程，仅做 FillCallback 与
//     原子置位，与 WASAPI 后端同一实时约束（无锁/无分配/无日志/无 JNI）。
//
// 生命周期：
//   - start(): openStream(阻塞) → requestStart → AAudioStream_waitForStateChange
//     等待进入 STARTED（阻塞等待，非轮询）。
//   - is_running(): 流已进入 STARTED 且未收到 disconnect/error。
//   - stop(): requestStop → close → delete builder。
//
// 格式支持：F32LE → AAUDIO_FORMAT_PCM_FLOAT，S16LE → AAUDIO_FORMAT_PCM_I16。
// 其余编码（S24/S32/U8）返回 unsupported（客户端格式转换见 AGENT.md §14，后续实现）。
class AaudioPlayback final : public PlaybackBackend {
public:
    AaudioPlayback() = default;
    ~AaudioPlayback() override;

    bool start(AudioFormat format, FillCallback cb) override;
    void stop() override;
    bool is_running() const override;

private:
    static aaudio_data_callback_result_t on_data_callback(AAudioStream* stream,
        void* user_data,
        void* audio_data,
        int32_t num_frames);
    static void on_error_callback(AAudioStream* stream, void* user_data, aaudio_result_t error);

    // 关闭并释放流与 builder（幂等，可重复调用）。
    void close_stream() noexcept;

    AAudioStreamBuilder* builder_ = nullptr;
    AAudioStream* stream_ = nullptr;
    FillCallback callback_;
    std::uint32_t frame_bytes_ = 0;

    // running_: 流已进入 STARTED 且未收到 disconnect/error。
    // start() 成功后置 true；stop() 或 error callback（disconnect）置 false。
    std::atomic<bool> running_ { false };
};

} // namespace aqua::audio

#endif // AQUA_AAUDIO_PLAYBACK_H
