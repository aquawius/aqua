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
#include "aqua/audio/devices/audio_device.h"
#include "aqua/audio/playback/audio_playback_config.h"
#include "aqua/compat/move_only_function.h"

#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <span>
#include <string_view>

namespace aqua::audio {

class AudioDeviceManager;

// 输出流实际运行参数（后端 open 后回读；仅供诊断/显示，不用于控制决策）。
// 各后端字段语义：
//   - AAudio：performance_mode 为 AAUDIO_PERFORMANCE_MODE_* 原始值（见下方统一
//     词汇常量，后端实现以 static_assert 锁定数值一致）；frames_per_burst =
//     设备原生 burst；buffer_capacity_frames = 缓冲最大容量（帧）。
//     不采集 buffer_size（不调 setBufferSizeInFrames，size 恒等于容量）与
    //     callback_frames（未设 setFramesPerCallback，回读恒为 unspecified）。
    //     device_id = open 后 getDeviceId() 回读（"android:N"；UNSPECIFIED 留空）。
//   - WASAPI：performance_mode 复用统一词汇（low_latency = IAudioClient3，
//     none = legacy IAudioClient）；frames_per_burst = 引擎基本周期（仅
//     IAudioClient3 可知，否则 0）；buffer_capacity_frames = 端点缓冲帧数；
//     device_id = 激活的 endpoint id（即所请求的设备，天然就是实际设备）。
struct AudioStreamInfo {
    enum class Backend : std::uint32_t { None = 0, AAudio = 1, Wasapi = 2 };

    // performance_mode 统一词汇：与 AAUDIO_PERFORMANCE_MODE_* 数值一致
    // （AAudio 后端以 static_assert 锁定），WASAPI 复用同一取值空间。
    static constexpr std::int32_t kPerformanceNone = 10;
    static constexpr std::int32_t kPerformancePowerSaving = 11;
    static constexpr std::int32_t kPerformanceLowLatency = 12;

    Backend backend = Backend::None;
    std::uint32_t sample_rate = 0; // 实际流采样率（AAudio 可能被系统 SRC）
    std::uint32_t channels = 0;
    std::int32_t performance_mode = 0;
    std::uint32_t frames_per_burst = 0;
    std::uint32_t buffer_capacity_frames = 0;
    // ---- 本次流运行期统计（Gauge：后端音频线程写，stream_info() 原子缓存读）----
    // 回答"WASAPI 没 callback / callback 了但 JB 没数据"这种二分问题。
    std::uint64_t callback_count = 0; // 后端实际回调次数（WASAPI 事件渲染趟 / AAudio data callback）
    std::uint32_t current_padding_frames = 0; // 端点缓冲当前填充帧数（WASAPI GetCurrentPadding 缓存；AAudio 未知=0）
    std::uint64_t xrun_count = 0; // 欠载/超限（AAudio xRun；WASAPI 暂不统计）
    // 实际输出设备的回读（playback_switching_design.md §8；空 = 未知/未上报）。
    // restart 事务的 previous_active_device 由此捕获。
    AudioDeviceId device_id;
};

// 诊断显示名（backend / performance 统一词汇的稳定字符串）。
[[nodiscard]] constexpr std::string_view audio_stream_backend_name(
    AudioStreamInfo::Backend backend) noexcept
{
    switch (backend) {
    case AudioStreamInfo::Backend::AAudio:
        return "aaudio";
    case AudioStreamInfo::Backend::Wasapi:
        return "wasapi";
    case AudioStreamInfo::Backend::None:
        break;
    }
    return "none";
}

[[nodiscard]] constexpr std::string_view audio_stream_performance_name(
    std::int32_t performance_mode) noexcept
{
    switch (performance_mode) {
    case AudioStreamInfo::kPerformanceNone:
        return "none";
    case AudioStreamInfo::kPerformanceLowLatency:
        return "low_latency";
    case AudioStreamInfo::kPerformancePowerSaving:
        return "power_saving";
    }
    return "unknown";
}

// 回放回调：后端需要数据时调用，由应用填充 output。
// 返回实际填充的帧数（每声道采样数）。回调状态经 lambda capture 传入。
// 类型经 compat 别名声明（MSVC = move_only_function；libc++ 回退 std::function）。
using AudioPlaybackCallback = compat::MoveOnlyFunction<std::uint32_t(std::span<std::byte>) noexcept>;

// 运行期事件回调：异步错误（DeviceDisconnected / BackendFailed 等）。
using AudioPlaybackEventCallback = compat::MoveOnlyFunction<void(AudioError) noexcept>;

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
        AudioPlaybackEventCallback event_callback = { }) noexcept = 0;

    // 当前是否已经进入运行状态。
    [[nodiscard]] virtual bool is_running() const noexcept = 0;

    // 回读输出流实际运行参数（start 成功前 / stop 后 backend=None）。
    // 线程安全：任意线程可调；值为 start 时缓存的原子近似读值。
    [[nodiscard]] virtual AudioStreamInfo stream_info() const noexcept
    {
        return { };
    }

    // 停止回放并等待回调线程退出。未运行时调用为 no-op。可再次 start()。
    // 不得从 callback / event_callback 内直接调用 stop()。
    virtual void stop() noexcept = 0;
};

// 工厂：创建当前平台的回放后端（如 WASAPI）。device_manager 必须在本对象生命周期内保持有效。
// 该平台尚未实现时返回 nullptr。
std::unique_ptr<AudioPlayback> create_playback(AudioDeviceManager& device_manager);

} // namespace aqua::audio

#endif // AQUA_AUDIO_PLAYBACK_H
