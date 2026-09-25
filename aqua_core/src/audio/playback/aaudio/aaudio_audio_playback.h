#ifndef AQUA_AUDIO_PLAYBACK_AAUDIO_AUDIO_PLAYBACK_H
#define AQUA_AUDIO_PLAYBACK_AAUDIO_AUDIO_PLAYBACK_H

// AAudio 回放后端（Android）。
//
// 设计决议见 aqua_core/doc/aaudio_backend_design.md：
//   - 格式协商：请求 server 契约格式；open 后回读实际配置，编码/声道必须
//     一致（字节布局硬约束），采样率允许系统 SRC（水位机制吸收漂移）；
//   - LOW_LATENCY/NONE + SHARED 由 AudioPlaybackConfig::low_latency 选择，
//     framesPerCallback 自适应（0 = 设备原生 burst）；
//   - data callback 内禁止 close（AAudio 死锁）：错误路径只返回
//     AAUDIO_CALLBACK_RESULT_STOP，close 由控制线程 stop() 执行；
//   - error callback 发布 pending error（原子，供 data callback 观察后 STOP）
//     并即时投递 event callback（与 WASAPI 事件线程对等）：运行期错误必须
//     立刻进入 ClientRuntime 的错误驱动恢复，否则流死后无人感知
//     （JB 打满、永久静音）。
//
// 流归属（generation）：本实例被 PlaybackManager 复用跨越多次 stop/start，
// 而 AAudio 的 error callback **不与 close() 同步**（close 只保证 data
// callback 已返回），所以「上一条流的迟到讣告」是真实存在的。回调只拿到
// AAudioStream* 与 user_data，指针本身不能当身份令牌——close 之后新流完全
// 可能分配到同一地址（ABA）。因此 user_data 指向 per-stream 的 StreamSlot，
// 由本类持有并携带 generation：迟到回调据此被判为旧流并丢弃，绝不触碰
// running_ / pending_error_ / event_callback_。
// 槽位保留策略见 retired_slot_ 的注释。
//
// 线程模型：AAudio 内部 realtime 线程驱动 data callback（本类不创建音频
// 线程）；stop() 保证回调退出后才返回（AAudioStream_requestStop +
// AAudioStream_close 的同步语义）。

#include "aqua/audio/audio_stream_event.h"
#include "aqua/audio/playback/audio_playback.h"

#include <aaudio/AAudio.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

namespace aqua::audio::aaudio {

class AAudioAudioPlayback final : public AudioPlayback {
public:
    explicit AAudioAudioPlayback(AudioDeviceManager& device_manager);
    ~AAudioAudioPlayback() override;

    AAudioAudioPlayback(const AAudioAudioPlayback&) = delete;
    AAudioAudioPlayback& operator=(const AAudioAudioPlayback&) = delete;

    std::expected<void, AudioError> start(
        const AudioPlaybackConfig& config,
        AudioPlaybackCallback callback,
        AudioPlaybackEventCallback event_callback = { }) noexcept override;

    [[nodiscard]] bool is_running() const noexcept override;

    [[nodiscard]] AudioStreamInfo stream_info() const noexcept override;

    [[nodiscard]] StreamGeneration generation() const noexcept override
    {
        return live_generation_.load(std::memory_order_acquire);
    }

    void stop() noexcept override;

private:
    // data callback 上下文：帧几何 + 应用回调。由所属 StreamSlot 持有。
    struct CallbackContext {
        AudioPlaybackCallback callback;
        // 帧几何（open 后回读，回调内只读）
        std::uint32_t frame_bytes = 0;
        // 数字静音字节（U8=0x80，其余=0x00；config.format.silence_byte()）
        std::byte silence_byte { 0 };
    };

    // 每条流的身份载体，作为 AAudio 的 user_data。回调只信任自己拿到的槽位，
    // 不从 this 上读「当前」状态——那正是流身份丢失的来源。
    struct StreamSlot {
        AAudioAudioPlayback* self = nullptr;
        StreamGeneration generation = kNoStreamGeneration;
        std::shared_ptr<CallbackContext> context;
    };

    static aaudio_data_callback_result_t on_data_callback(
        AAudioStream* stream, void* user_data, void* audio_data, int32_t num_frames) noexcept;

    static void on_error_callback(
        AAudioStream* stream, void* user_data, aaudio_result_t error) noexcept;

    void publish_error(AudioError error) noexcept;

    // 运行期致命错误的一次性上报（error callback 线程 / data callback 异常
    // 路径共用）：发布 pending error（data callback 观察后返回 STOP）、
    // 将 running_ 置 false（is_running 如实反映流已死）、并即时调用
    // event_callback_ 进入错误驱动恢复。fatal_reported_ 保证每次 start
    // 生命周期内只投递一次；stop() 据此跳过重复投递。
    //
    // slot->generation 与 live_generation_ 不符时直接返回：那是已退役流的
    // 迟到讣告，绝不能让它把复用了本实例的新流标记为已死。
    void report_fatal_once(const StreamSlot& slot, AudioError error) noexcept;

    AudioDeviceManager& device_manager_;

    AAudioStream* stream_ = nullptr;

    // ---- 流身份 ----
    // generation_counter_ 单调递增，永不回绕复用；live_generation_ 是「当前
    // 被认领的流」，无流时为 kNoStreamGeneration。stop() 的**第一件事**就是
    // 把 live_generation_ 清零，此后任何迟到回调都在 generation 检查处被挡下，
    // 不会碰到下面即将被清空的 event_callback_（TOCTOU）。
    std::atomic<StreamGeneration> generation_counter_ { kNoStreamGeneration };
    std::atomic<StreamGeneration> live_generation_ { kNoStreamGeneration };
    // 当前流的槽位（user_data 指向它）。start() 创建，stop() 在 close 返回后
    // 降级为 retired。
    std::unique_ptr<StreamSlot> current_slot_;
    // 上一条流的槽位，保留到下一次 stop()。
    // 为什么必须保留：AAudio 的 error callback 不与 close() 同步，close 返回后
    // 仍可能有一次迟到投递，其 user_data 指向这块内存。保留一个世代即可覆盖
    // ——要出现「隔两代的迟到回调」，得跨过两次完整的 close()，而 close() 已经
    // 完成了流的销毁。有界（恒为 1 个槽位）且无 ABA。
    std::unique_ptr<StreamSlot> retired_slot_;
    // 迟到讣告计数（诊断）：非零即证明 provenance 过滤真的拦下了东西。
    std::atomic<std::uint64_t> stale_events_dropped_ { 0 };

    // 运行期事件回调。线程安全论证：写入只发生在 start()（openStream 之前，
    // 回调尚不可能触发）与 stop()（先清 live_generation_ 挡住迟到回调，再
    // AAudioStream_close 等待 data callback 退出）两条控制路径；流存活期间
    // 无写者，error/data callback 线程可安全读取调用。
    AudioPlaybackEventCallback event_callback_;
    std::atomic<AudioError> pending_error_ { AudioError::None };
    // 本次 start 生命周期内致命错误是否已即时投递（stop() 据此去重）。
    std::atomic<bool> fatal_reported_ { false };

    std::atomic<bool> running_ { false };

    // stream_info() 原子缓存：start() 在控制线程写入（open 后回读一次），
    // stop() 清零；任意线程 relaxed 读，近似一致性满足诊断用途。
    std::atomic<std::uint32_t> info_sample_rate_ { 0 };
    std::atomic<std::uint32_t> info_channels_ { 0 };
    std::atomic<std::int32_t> info_performance_mode_ { 0 };
    std::atomic<std::uint32_t> info_frames_per_burst_ { 0 };
    std::atomic<std::uint32_t> info_buffer_capacity_ { 0 };
    // 运行期统计（data callback 线程写，stream_info() relaxed 读）：
    // callback 数每 data callback 递增；xrun 每 64 次回调采样一次
    // AAudioStream_getXRunCount（RT 上节流，避免每次回调做 native 调用）。
    std::atomic<std::uint64_t> stats_callback_count_ { 0 };
    std::atomic<std::uint64_t> stats_xrun_count_ { 0 };

    // 实际输出设备回读（"android:N"；playback_switching_design.md §8）：
    // 字符串不能原子化，mutex 保护诊断冷路径。
    mutable std::mutex info_device_mutex_;
    std::string info_device_id_;
};

} // namespace aqua::audio::aaudio

#endif // AQUA_AUDIO_PLAYBACK_AAUDIO_AUDIO_PLAYBACK_H
