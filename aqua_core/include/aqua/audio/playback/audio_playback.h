#ifndef AQUA_AUDIO_PLAYBACK_H
#define AQUA_AUDIO_PLAYBACK_H

// 输出流抽象（应用 --pull--> OS）。
//
// 与采集相反，回放采用"后端索取数据"（pull）而非"应用推送数据"（write）：
// 后端在需要数据时调用回调，应用把数据填入 output 并返回实际填充的帧数。
// 这更贴近底层 realtime audio API（WASAPI/AAudio 的 pull 模型），
// 也让低延迟播放可以由应用侧的音频队列驱动。
//
// 线程模型与回调生命周期（实现必须遵守）：
//   - 回调运行在后端的实时音频线程上，禁止阻塞（不得加锁/堆分配/IO）；
//   - output 仅在回调内有效；
//   - output 的长度始终是整数个 sample frame；callback 返回实际写入的帧数。
//     实现应保证 [returned_frames * frame_bytes, output.size()) 为静音，避免
//     backend 继续消费上一次回调残留数据；返回值不得大于 output 可容纳的帧数；
//   - 回调签名 noexcept：任何异常都不得越过回调（实现负责捕获并记录）；
//   - stop() 返回时保证回调不再被调用；stop() 后可再次 start()。
//
// 运行期错误（设备被拔出 / 音频服务重启等）通过 event_callback 投递，
// 语义与 AudioCaptureEventCallback 一致（见 audio_capture.h）。

#include "aqua/audio/audio_error.h"
#include "aqua/audio/playback/audio_playback_config.h"

#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <span>

namespace aqua::audio {

class AudioDeviceManager;

// 回放回调：后端需要数据时调用，由应用填充 output。
// 返回实际填充的帧数（每声道采样数）。回调状态经 lambda capture 传入。
using AudioPlaybackCallback = std::move_only_function<std::uint32_t(std::span<std::byte>) noexcept>;

// 运行期事件回调：异步错误（DeviceDisconnected / BackendFailed 等）。
using AudioPlaybackEventCallback = std::move_only_function<void(AudioError) noexcept>;

// 输出流抽象。跨平台接口，具体实现见 src/audio/playback/<backend>/。
class AudioPlayback {
public:
    virtual ~AudioPlayback() = default;

    // 以 config 启动回放；回调在需要数据时被调用。
    // callback 为空或 config 非法返回 InvalidArgument；
    // 其余失败类别同 AudioCapture::start（含 FormatUnsupported：设备不支持 config.format）。
    // 成功返回后，回调收到的 output 以 config.format 解释并填充对应字节。
    virtual std::expected<void, AudioError>
    start(const AudioPlaybackConfig& config,
        AudioPlaybackCallback callback,
        AudioPlaybackEventCallback event_callback = {}) noexcept = 0;

    // 当前是否已经进入运行状态。
    [[nodiscard]] virtual bool is_running() noexcept = 0;

    // 停止回放并等待回调线程退出。未运行时调用为 no-op。可再次 start()。
    // 不得从 callback / event_callback 内直接调用 stop()。
    virtual void stop() noexcept = 0;
};

// 工厂：创建当前平台的回放后端（如 WASAPI）。device_manager 必须在本对象生命周期内保持有效。
// 该平台尚未实现时返回 nullptr。
std::unique_ptr<AudioPlayback> create_playback(AudioDeviceManager& device_manager);

} // namespace aqua::audio

#endif // AQUA_AUDIO_PLAYBACK_H
