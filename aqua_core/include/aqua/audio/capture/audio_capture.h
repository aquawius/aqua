#ifndef AQUA_AUDIO_CAPTURE_H
#define AQUA_AUDIO_CAPTURE_H

// 输入流抽象（OS --push--> 应用）。
//
// 线程模型与回调生命周期（实现必须遵守）：
//   - block_callback 运行在后端的实时音频线程上，禁止阻塞（不得加锁/堆分配/IO）；
//   - block.data 仅在回调内有效，需要保留的数据必须自行拷贝；
//   - 回调签名 noexcept：任何异常都不得越过回调（实现负责捕获并记录）；
//   - start() 成功前不会触发回调；stop() 返回时保证回调不再被调用
//     （实现需等待音频线程退出后再返回）；
//   - stop() 后可再次 start()（同一实例复用）。
//   - 控制 API（start/stop/is_running/info）要求由同一个 control thread 调用，
//     不要求多个线程并发调用；backend 的实时音频线程与 event thread 不调用这些 API。
//
// 运行期错误（设备被拔出 / 音频服务重启等）通过 event_callback 投递，
// 与 start() 的同步返回值互补（start() 只覆盖启动期失败）：
//   - event_callback 运行在 backend 的内部线程（已退出实时音频数据路径），
//     不是调用 start()/stop() 的控制线程，回调必须快速返回；
//   - 不得在 block_callback / event_callback 内调用 stop() 或 start()
//     （stop() 会 join 该线程导致自死锁）；
//   - event_callback 为 nullptr 时，运行期错误仅记日志，上层可通过
//     is_running() 轮询感知流已停止。

#include "aqua/audio/audio_block.h"
#include "aqua/audio/audio_error.h"
#include "aqua/audio/capture/audio_capture_config.h"

#include <cstdint>
#include <expected>
#include <functional>
#include <memory>

namespace aqua::audio {

class AudioDeviceManager;

// 采集块回调：后端在实时音频线程上推入一块变长 PCM（AudioBlock）。
// 回调状态经 lambda capture 传入（替代原 user_data 参数）。
using AudioCaptureCallback = std::move_only_function<void(const AudioBlock&) noexcept>;

// 运行期事件回调：异步错误（DeviceDisconnected / BackendFailed 等）。
using AudioCaptureEventCallback = std::move_only_function<void(AudioError) noexcept>;

// 已启动 AudioCapture 的实际流信息。
// format 是该音频流的权威格式：当 AudioCaptureConfig::format 未指定时，
// backend 在创建 stream 后解析得到的实际/shared-mode 格式会写入这里。
struct AudioCaptureInfo {
    AudioFormat format;
    std::uint32_t frames_per_buffer = 0;
};

// Capture 时间轴状态（由 packet flags 与事件饥饿 fallback 推导）。
enum class AudioCaptureState : std::uint8_t {
    Active,  // 最近一次真实 packet 非 SILENT
    Silent,  // 最近一次真实 packet 带 SILENT flag，engine 时间轴仍在推进
    Starved, // 事件超时且无 packet，backend 正在生成合成静音维持时间轴
};

[[nodiscard]] constexpr const char* capture_state_name(AudioCaptureState state) noexcept
{
    switch (state) {
    case AudioCaptureState::Active:
        return "active";
    case AudioCaptureState::Silent:
        return "silent";
    case AudioCaptureState::Starved:
        return "starved";
    }
    return "unknown";
}

// Runtime diagnostics for the capture backend. Counters are cumulative for the
// current backend lifetime and are intended for troubleshooting only.
struct AudioCaptureStats {
    std::uint64_t audio_events = 0;
    std::uint64_t packet_queries = 0;
    std::uint64_t packet_empty = 0;
    std::uint64_t packets_ready = 0;
    std::uint64_t get_buffer_success = 0;
    std::uint64_t callbacks = 0;
    std::uint64_t silent_callbacks = 0;
    // 事件饥饿 fallback（loopback quiescence 等）相关统计。
    std::uint64_t synthetic_silence_blocks = 0;
    std::uint64_t generated_silence_frames = 0;
    std::uint64_t starved_events = 0; // 进入 starved 状态的次数（连续 2 次超时起算）
    std::uint64_t starved_ms = 0;     // 累计处于 starved 状态的时长
    AudioCaptureState state = AudioCaptureState::Active;
};

// 输入流抽象。跨平台接口，具体实现见 src/audio/capture/<backend>/。
class AudioCapture {
public:
    virtual ~AudioCapture() = default;

    // 以 config 启动采集。
    // block_callback 为空或 config 非法 -> InvalidArgument。
    // config.format == nullopt 时由 backend 选择并报告实际 stream format；
    // 指定 format 时必须由 backend 原生支持，否则返回 FormatUnsupported。
    virtual std::expected<void, AudioError>
    start(const AudioCaptureConfig& config,
        AudioCaptureCallback block_callback,
        AudioCaptureEventCallback event_callback = {}) noexcept = 0;

    // start() 成功后返回当前音频流的实际信息。未运行时返回上一次成功 start() 的信息，
    // 若实例从未成功启动，则为默认值。
    [[nodiscard]] virtual const AudioCaptureInfo& info() const noexcept = 0;

    [[nodiscard]] virtual bool is_running() const noexcept = 0;

    // Backend runtime diagnostics. Implementations may return zeroed stats when
    // no diagnostics are available.
    [[nodiscard]] virtual AudioCaptureStats stats() const noexcept { return {}; }

    // 停止采集并等待回调线程退出。未运行时调用为 no-op。可再次 start()。
    // 不得从 frame_callback / event_callback 内直接调用 stop()。
    virtual void stop() noexcept = 0;
};

// 工厂：创建当前平台的采集后端（如 WASAPI）。device_manager 必须在本对象生命周期内保持有效。
// 该平台尚未实现时返回 nullptr。
std::unique_ptr<AudioCapture> create_capture(AudioDeviceManager& device_manager);

} // namespace aqua::audio

#endif // AQUA_AUDIO_CAPTURE_H
