#ifndef AQUA_AUDIO_PLAYBACK_AUDIO_PLAYBACK_H
#define AQUA_AUDIO_PLAYBACK_AUDIO_PLAYBACK_H

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
// 语义与 AudioCaptureEventCallback 一致（见 audio_capture.h）。事件必须带
// stream generation（audio_stream_event.h）：manager 复用同一个 backend
// 实例跨越多次 stop/start，无归属的错误在上层不可判别。

#include "aqua/audio/audio_error.h"
#include "aqua/audio/audio_stream_event.h"
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
//     不采集 buffer_size（不调 setBufferSizeInFrames；AAudio 并不承诺 size == 容量，
//     该值只进启动日志供真机核对）与 callback_frames（未设 setFramesPerCallback，
//     回读恒为 unspecified）。
//     device_id = open 后 getDeviceId() 回读（"android:N"；UNSPECIFIED 留空）。
//   - WASAPI：performance_mode 复用统一词汇（low_latency = IAudioClient3，
//     none = legacy IAudioClient）；frames_per_burst = **选定的引擎周期**
//     （2026-10-19 修正：此前存的是 fundamental 基本周期，与真正传给
//     InitializeSharedAudioStream 的 period_frames 可能差数倍，几何地板的
//     回退值因此算错；fundamental 现由 engine_period_default_frames 承担）；
//     buffer_capacity_frames = 端点缓冲帧数；
//     device_id = 激活的 endpoint id（即所请求的设备，天然就是实际设备）。
struct AudioStreamInfo {
    enum class Backend : std::uint32_t { None = 0,
        AAudio = 1,
        Wasapi = 2 };

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
    // ---- 输出侧延迟（2026-10-19 补）----
    // buffer_size_frames：后端**实际**的缓冲帧数。
    //   WASAPI = 端点缓冲（与 buffer_capacity_frames 同值）；
    //   AAudio = AAudioStream_getBufferSizeInFrames —— **真正决定 AAudio 输出延迟
    //   的是它，不是 capacity**（capacity 只是天花板）。此前它只进启动日志，
    //   运行期完全不可见 ⇒ Android 上没有任何字段能回答"输出侧压了多少"。
    //   0 = 未上报。
    std::uint32_t buffer_size_frames = 0;
    // output_buffering_frames：**此刻**压在输出设备缓冲里的帧数（输出侧延迟地板）。
    //   WASAPI = GetCurrentPadding（端点缓冲里还没被设备消费的帧数）；
    //   AAudio = 已写入帧数 − 设备已播位置（getTimestamp 的 framePosition）。
    //   两者同在"采样帧"这条时间轴上，无需时钟标定；除以采样率即毫秒。
    //   0 = 未上报（设备没起来 / 后端不提供该读数）。
    std::uint32_t output_buffering_frames = 0;
    // ---- 引擎周期与格式协商（2026-10-19 补；一次性快照，start 时写）----
    // frames_per_burst 历史上在 WASAPI 存的是 fundamental_period（引擎基本周期），
    // 而真正传给 InitializeSharedAudioStream 的是 period_frames（fundamental 的整数倍，
    // requested==0 时通常大好几倍）——几何地板拿它当"引擎周期"会差数倍。
    // 修正：frames_per_burst 改存**选定周期**，fundamental 由 engine_period_default 承担。
    std::uint32_t engine_period_frames = 0; // 选定周期（= 修正后的 frames_per_burst）
    std::uint32_t engine_period_min_frames = 0; // 设备允许的最小周期（0 = 未上报）
    std::uint32_t engine_period_max_frames = 0; // 设备允许的最大周期（0 = 未上报）
    std::uint32_t engine_period_default_frames = 0; // 引擎默认周期（0 = 未上报）
    std::uint32_t engine_period_fundamental_frames = 0; // 引擎基本周期（选定周期的粒度单位）
    bool engine_resampling = false; // 端点混音格式的**采样率或声道数**与请求格式
        // 不同 ⇒ 引擎要做重采样 / 混音转换（Windows 的 SRC 在线路里，音频发飘 /
        // 发晚的头号原因）。纯位深/编码差异（请求 F32、端点 S16）**不**置位——
        // 那是等精度转换，不引起音高或延迟问题。
    std::uint32_t mix_sample_rate = 0; // 端点混音格式采样率（0 = 未上报）
    std::uint32_t mix_channels = 0; // 端点混音格式声道数（0 = 未上报）
    // ---- 本次流运行期统计（Gauge：后端音频线程写，stream_info() 原子缓存读）----
    // 回答"WASAPI 没 callback / callback 了但 JB 没数据"这种二分问题。
    std::uint64_t callback_count = 0; // 后端实际回调次数（WASAPI 事件渲染趟 / AAudio data callback）
    std::uint32_t current_padding_frames = 0; // 端点缓冲当前填充帧数（WASAPI GetCurrentPadding 缓存；AAudio 未知=0）
    std::uint64_t xrun_count = 0; // 欠载/超限（AAudio xRun；WASAPI 暂不统计）
    // ---- 2026-10-19 补：WASAPI 侧此前完全没有 underrun 计数 ----
    std::uint64_t empty_event_skips = 0; // 事件到了但端点缓冲**已满**、无事可写而
        // 跳过渲染的次数。⚠️ **它不是 underrun**：缓冲满 = 设备还没消费完 = 我们
        // 跑在设备前面（输出延迟顶到上限）。真正的"我们迟到了"信号是
        // **catchup_frames 偏大**（设备已经把缓冲吃空，我们必须一次补一大块）。
        // AAudio 侧用 xrun_count（那边确实表示欠载），两边语义不同、不可互换。
    std::uint32_t catchup_frames = 0; // 上一次事件时可写的帧数（≈ 迟到多少帧；
        // 正常 ≈ 引擎周期，**远大于周期即"我们迟到了"**）
    std::uint32_t catchup_frames_max = 0; // 本次运行最大可写帧数
    // ---- 2026-10-20 补：回调自身的开销（RT 线程两次 steady_clock 读）----
    // 单位是 **ns**（不是 ms：音频格式会变，ms↔帧的换算随之变化，内部量一律用
    // 绝对时间/帧数）。"这一拍能不能更短"只有这组数能回答：
    // cost 配引擎周期才是判据 —— cost=1ms@period=3.6ms 安全，@period=1.2ms 每拍都迟到。
    std::uint64_t callback_cost_ns = 0; // 最近一次 frame_callback_ 的耗时
    std::uint64_t callback_cost_max_ns = 0; // 本次运行峰值
    std::uint64_t callback_overruns = 0; // cost > 引擎周期 的次数（真正的"这一拍来不及"）
    // ---- 2026-10-20 补：IAudioClock（WASAPI 端点自己报的时钟）----
    // 与"输出已压帧数"是两条独立证据：一个是我们的记账差，一个是端点自报。
    // 采样在 audio 线程（COM 对象的所有者），诊断线程只读原子缓存。
    std::uint64_t clock_adjusted_latency_ns = 0; // 端点自报的输出延迟（0 = 未上报）
    std::uint64_t clock_position_frames = 0; // 设备播放位置（每 16 拍取一次）
    std::uint64_t clock_frequency_hz = 0; // 设备时钟频率（算漂移 ∝ 位置/时间 的分母）
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

// 运行期事件回调：异步错误（DeviceDisconnected / BackendFailed 等），
// 带 stream generation（见 audio_stream_event.h）。
using AudioPlaybackEventCallback = AudioStreamEventCallback;

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

    // 当前流的代号（每次成功 start() 递增；无流时为 kNoStreamGeneration）。
    // manager 用它判别 event 的归属，也用它把「刚起的流立刻死掉」识别为
    // 路由未稳定而非设备丢失。纯虚：漏实现会让所有事件被判为无归属而丢弃，
    // 这种失效必须编译期就暴露。
    // 线程安全：任意线程可调。
    [[nodiscard]] virtual StreamGeneration generation() const noexcept = 0;

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

#endif // AQUA_AUDIO_PLAYBACK_AUDIO_PLAYBACK_H
