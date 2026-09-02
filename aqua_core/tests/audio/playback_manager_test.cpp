// PlaybackManager restart 事务专项测试（playback_switching_design.md §11 Phase A-0）。
//
// 覆盖三个底线验证（全部使用 mock 后端 + 真实 JitterBuffer）：
//   1. RestartWhileCallbackActive：回调线程正在消费 JB 时发起 restart，
//      验证无死锁、无双重消费（break-before-make 的 SPSC 交接）；
//   2. JitterBufferSequenceContinuity：restart 前后 playhead 不重置、
//      不重新 pre-roll、诊断计数累计；
//   3. RestartUnderrunRecovery：restart 间隙（300ms 设备打开耗时）中 JB 排空，
//      新流接上后 PLC/FILL 正常恢复、无 reanchor、无会话重启。
//
// RuntimeState 不受 restart 影响这一点由实现结构保证：restart 只调用
// PlaybackManager 自身方法，不触碰 ClientRuntime 状态机（代码评审项）。

#include "aqua/audio/buffer/jitter_buffer.h"
#include "aqua/audio/playback/audio_playback.h"
#include "aqua/runtime/playback_manager.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <expected>
#include <functional>
#include <future>
#include <memory>
#include <span>
#include <thread>
#include <vector>

namespace aqua::runtime {
namespace {

using audio::AudioError;
using audio::AudioFrame;
using audio::AudioFormat;
using audio::AudioPlayback;
using audio::AudioPlaybackCallback;
using audio::AudioPlaybackConfig;
using audio::AudioPlaybackEventCallback;
using audio::JitterBuffer;
using audio::JitterBufferConfig;

constexpr std::uint32_t kFrameCount = 480; // F：每槽 sample frame 数
constexpr std::uint32_t kCapacitySlots = 30;
constexpr std::uint32_t kPrePushSlots = 23; // 76% 水位，稳态 normal 区间

AudioFormat make_format()
{
    AudioFormat format;
    format.encoding = audio::AudioEncoding::PCM_S16LE;
    format.channels = 1;
    format.sample_rate = 48000;
    return format;
}

std::shared_ptr<JitterBuffer> make_jitter_buffer()
{
    JitterBufferConfig cfg;
    cfg.capacity_slots = kCapacitySlots;
    cfg.format = make_format();
    cfg.frame_count = kFrameCount;
    auto jb = JitterBuffer::create(cfg);
    if (!jb) {
        ADD_FAILURE() << "JitterBuffer create failed";
        return nullptr;
    }
    return std::shared_ptr<JitterBuffer>(std::move(*jb));
}

// 生成一帧 PCM：首 sample 编码序号（S16LE），其余为 0。
std::vector<std::byte> make_frame_payload(std::uint64_t sequence)
{
    std::vector<std::byte> data(kFrameCount * 2, std::byte { 0 });
    const auto value = static_cast<std::uint16_t>(sequence);
    data[0] = static_cast<std::byte>(value & 0xFF);
    data[1] = static_cast<std::byte>(value >> 8);
    return data;
}

// 从回调 output 解回首 sample 编码的序号（0 = 静音）。
std::uint64_t decode_seq(std::span<const std::byte> output)
{
    if (output.size() < 2) {
        return 0;
    }
    return static_cast<std::uint64_t>(
        static_cast<std::uint16_t>(static_cast<std::uint8_t>(output[0]))
        | (static_cast<std::uint16_t>(static_cast<std::uint8_t>(output[1])) << 8));
}

bool push_frame(JitterBuffer& jb, std::uint64_t sequence)
{
    const auto payload = make_frame_payload(sequence);
    const AudioFrame frame { sequence, kFrameCount, payload };
    return jb.push(frame);
}

// 可编排的回放后端 mock：
//   - threaded 模式：后台线程按 interval 周期调用回调（模拟真实音频线程）；
//   - manual 模式：测试在同一线程手动 invoke_callback()（确定性时序）。
// stop() 遵守 AudioPlayback 契约：join 回调线程后才返回。
class MockAudioPlayback final : public AudioPlayback {
public:
    struct Behavior {
        bool threaded = true;
        std::chrono::milliseconds start_delay { 0 }; // start() 内耗时（模拟设备打开）
        std::chrono::milliseconds pull_interval { 1 }; // 回调节奏
        std::uint32_t frames_per_callback = kFrameCount;
    };

    explicit MockAudioPlayback(Behavior behavior)
        : behavior_(behavior)
    {
    }

    ~MockAudioPlayback() override { stop(); }

    std::expected<void, AudioError> start(const AudioPlaybackConfig& config,
        AudioPlaybackCallback callback,
        AudioPlaybackEventCallback event_callback) noexcept override
    {
        if (running_.load(std::memory_order_acquire)) {
            return std::unexpected(AudioError::AlreadyRunning);
        }
        if (!callback) {
            return std::unexpected(AudioError::InvalidArgument);
        }
        if (behavior_.start_delay > std::chrono::milliseconds::zero()) {
            std::this_thread::sleep_for(behavior_.start_delay);
        }
        config_ = config;
        callback_ = std::move(callback);
        event_callback_ = std::move(event_callback);
        output_.assign(
            static_cast<std::size_t>(behavior_.frames_per_callback)
                * config.format.frame_bytes(),
            std::byte { 0 });
        stop_flag_.store(false, std::memory_order_release);
        start_calls_.fetch_add(1, std::memory_order_relaxed);
        running_.store(true, std::memory_order_release);
        if (behavior_.threaded) {
            thread_ = std::thread(&MockAudioPlayback::thread_main, this);
        }
        return { };
    }

    bool is_running() const noexcept override
    {
        return running_.load(std::memory_order_acquire);
    }

    audio::AudioStreamInfo stream_info() const noexcept override
    {
        if (!running_.load(std::memory_order_acquire)) {
            return { };
        }
        audio::AudioStreamInfo info;
        info.backend = audio::AudioStreamInfo::Backend::Wasapi;
        info.sample_rate = config_.format.sample_rate;
        info.channels = config_.format.channels;
        info.performance_mode = audio::AudioStreamInfo::kPerformanceNone;
        info.frames_per_burst = behavior_.frames_per_callback;
        info.buffer_capacity_frames = behavior_.frames_per_callback;
        return info;
    }

    void stop() noexcept override
    {
        stop_flag_.store(true, std::memory_order_release);
        if (thread_.joinable() && thread_.get_id() != std::this_thread::get_id()) {
            thread_.join();
        }
        const bool was_running = running_.exchange(false, std::memory_order_acq_rel);
        if (was_running) {
            stop_calls_.fetch_add(1, std::memory_order_relaxed);
        }
        callback_ = nullptr;
        event_callback_ = nullptr;
    }

    // manual 模式：驱动一次回调（返回填充帧数）。
    std::uint32_t invoke_callback() noexcept
    {
        if (!callback_) {
            return 0;
        }
        return callback_(std::span<std::byte>(output_));
    }

    [[nodiscard]] std::uint64_t start_calls() const noexcept
    {
        return start_calls_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t stop_calls() const noexcept
    {
        return stop_calls_.load(std::memory_order_relaxed);
    }
    // 并发回调观测：> 1 说明出现双重消费（两个回调线程同时存活）。
    [[nodiscard]] int max_concurrent_callbacks() const noexcept
    {
        return max_concurrent_.load(std::memory_order_relaxed);
    }

private:
    void thread_main() noexcept
    {
        while (!stop_flag_.load(std::memory_order_acquire)) {
            const int entered = ++concurrent_;
            int observed = max_concurrent_.load(std::memory_order_relaxed);
            while (entered > observed
                && !max_concurrent_.compare_exchange_weak(
                       observed, entered, std::memory_order_relaxed)) {
            }
            if (callback_) {
                (void)callback_(std::span<std::byte>(output_));
            }
            --concurrent_;
            std::this_thread::sleep_for(behavior_.pull_interval);
        }
    }

    Behavior behavior_;
    AudioPlaybackConfig config_;
    AudioPlaybackCallback callback_;
    AudioPlaybackEventCallback event_callback_;
    std::vector<std::byte> output_;
    std::thread thread_;
    std::atomic<bool> running_ { false };
    std::atomic<bool> stop_flag_ { false };
    std::atomic<int> concurrent_ { 0 };
    std::atomic<int> max_concurrent_ { 0 };
    std::atomic<std::uint64_t> start_calls_ { 0 };
    std::atomic<std::uint64_t> stop_calls_ { 0 };
};

AudioPlaybackConfig make_playback_config()
{
    AudioPlaybackConfig config;
    config.format = make_format();
    config.frames_per_buffer = kFrameCount;
    return config;
}

// 轮询等待条件成立（默认 2s 超时）。
bool wait_for(const std::function<bool()>& predicate,
    std::chrono::milliseconds timeout = std::chrono::milliseconds(2000))
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return predicate();
}

// ---- 基础状态契约 ----

TEST(PlaybackManagerTest, StartStopStateTransitions)
{
    auto mock = std::make_unique<MockAudioPlayback>(
        MockAudioPlayback::Behavior { .threaded = false });
    auto* mock_ptr = mock.get();
    PlaybackManager manager(std::move(mock));

    EXPECT_EQ(manager.state(), PlaybackState::Inactive);
    EXPECT_FALSE(manager.is_running());
    EXPECT_TRUE(manager.available());

    // 尚未 start 过：restart 无"旧配置"，拒绝。
    const auto early = manager.restart();
    ASSERT_FALSE(early.has_value());
    EXPECT_EQ(early.error(), AudioError::NotRunning);

    const auto started = manager.start(make_playback_config(),
        [](std::span<std::byte>) noexcept { return 0U; });
    ASSERT_TRUE(started.has_value());
    EXPECT_EQ(manager.state(), PlaybackState::Running);
    EXPECT_TRUE(manager.is_running());

    manager.stop();
    EXPECT_EQ(manager.state(), PlaybackState::Inactive);
    EXPECT_FALSE(manager.is_running());
    EXPECT_EQ(mock_ptr->stop_calls(), 1U);
}

TEST(PlaybackManagerTest, StartWithEmptyCallbackRejected)
{
    PlaybackManager manager(std::make_unique<MockAudioPlayback>(
        MockAudioPlayback::Behavior { .threaded = false }));
    const auto result = manager.start(make_playback_config(), nullptr);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), AudioError::InvalidArgument);
    EXPECT_EQ(manager.state(), PlaybackState::Inactive);
}

// ---- Test 1：restart while callback active ----

TEST(PlaybackManagerRestartTest, RestartWhileCallbackActiveIsSafe)
{
    auto jb = make_jitter_buffer();
    ASSERT_NE(jb, nullptr);
    for (std::uint64_t seq = 1; seq <= kPrePushSlots; ++seq) {
        ASSERT_TRUE(push_frame(*jb, seq));
    }

    auto mock = std::make_unique<MockAudioPlayback>(MockAudioPlayback::Behavior { });
    auto* mock_ptr = mock.get();
    PlaybackManager manager(std::move(mock));

    std::atomic<std::uint64_t> last_seq { 0 };
    const auto started = manager.start(make_playback_config(),
        [&jb, &last_seq](std::span<std::byte> output) noexcept {
            const auto result = jb->pull(output);
            const auto seq = decode_seq(output);
            if (seq != 0) {
                last_seq.store(seq, std::memory_order_release);
            }
            return result.frames_filled;
        });
    ASSERT_TRUE(started.has_value());

    // producer：持续补充 seq 递增的帧（与消费同节奏）。
    std::atomic<bool> produce { true };
    std::atomic<std::uint64_t> producer_seq { kPrePushSlots };
    std::thread producer([&]() {
        while (produce.load(std::memory_order_acquire)) {
            const auto seq = producer_seq.fetch_add(1, std::memory_order_relaxed) + 1;
            (void)push_frame(*jb, seq);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });

    // 等回调线程真正跑起来（回调正在 JB.pop 的竞争窗口内发起 restart）。
    ASSERT_TRUE(wait_for([&] { return jb->pull_calls() >= 20; }))
        << "playback callback did not start consuming";

    const auto seq_before_restart = last_seq.load(std::memory_order_acquire);
    const auto frames_before_restart = jb->pull_frames();

    // 控制线程发起 restart；死锁则超时失败（测试的底线目标）。
    auto restart_future = std::async(std::launch::async, [&] { return manager.restart(); });
    ASSERT_NE(restart_future.wait_for(std::chrono::seconds(5)), std::future_status::timeout)
        << "restart deadlocked while callback active";
    const auto restarted = restart_future.get();
    ASSERT_TRUE(restarted.has_value()) << audio::audio_error_name(restarted.error());
    EXPECT_EQ(manager.state(), PlaybackState::Running);
    EXPECT_TRUE(manager.is_running());
    EXPECT_EQ(mock_ptr->start_calls(), 2U);

    // 无双重消费：任何时刻至多一个回调线程在拉 JB。
    ASSERT_TRUE(wait_for([&] { return last_seq.load() > seq_before_restart; }))
        << "no data consumed after restart";
    // mock 线程已稳定运行一段时间后再校验并发峰值（避免刚启动的窗口）。
    ASSERT_TRUE(wait_for([&] { return jb->pull_calls() >= 40; }));
    EXPECT_EQ(mock_ptr->max_concurrent_callbacks(), 1);
    EXPECT_GT(jb->pull_frames(), frames_before_restart);
    // 单调序号持续前进且无 reanchor：restart 后消费继续而非重置。
    EXPECT_EQ(jb->reanchor_count(), 0U);

    produce.store(false, std::memory_order_release);
    producer.join();
    manager.stop();
}

// ---- Test 2：JB sequence continuity ----

TEST(PlaybackManagerRestartTest, JitterBufferSequenceContinuityAcrossRestart)
{
    auto jb = make_jitter_buffer();
    ASSERT_NE(jb, nullptr);

    auto mock = std::make_unique<MockAudioPlayback>(
        MockAudioPlayback::Behavior { .threaded = false });
    auto* mock_ptr = mock.get();
    PlaybackManager manager(std::move(mock));

    // pre-roll：先填到 target 之上，首个回调即可建立锚点并消费真实数据。
    for (std::uint64_t seq = 1; seq <= kPrePushSlots; ++seq) {
        ASSERT_TRUE(push_frame(*jb, seq));
    }

    std::vector<std::uint64_t> consumed;
    const auto started = manager.start(make_playback_config(),
        [&jb, &consumed](std::span<std::byte> output) noexcept {
            const auto result = jb->pull(output);
            consumed.push_back(decode_seq(output));
            return result.frames_filled;
        });
    ASSERT_TRUE(started.has_value());
    ASSERT_EQ(manager.state(), PlaybackState::Running);

    // restart 前消费 seq 1..3（水位 23 -> 20，全程 normal 区间，无 Fill/Drop）。
    ASSERT_EQ(mock_ptr->invoke_callback(), kFrameCount);
    ASSERT_EQ(mock_ptr->invoke_callback(), kFrameCount);
    ASSERT_EQ(mock_ptr->invoke_callback(), kFrameCount);
    ASSERT_EQ(consumed, (std::vector<std::uint64_t> { 1, 2, 3 }))
        << "pre-restart pulls should consume seq 1..3";

    const auto restart_result = manager.restart();
    ASSERT_TRUE(restart_result.has_value());
    EXPECT_EQ(manager.state(), PlaybackState::Running);
    EXPECT_EQ(mock_ptr->start_calls(), 2U);

    // restart 后消费必须从 seq 4 继续：playhead 不重置、不重新 pre-roll
    // （重置会表现为 0（静音 hold）或 1（重新锚定到最老帧））。
    ASSERT_EQ(mock_ptr->invoke_callback(), kFrameCount);
    ASSERT_EQ(mock_ptr->invoke_callback(), kFrameCount);
    ASSERT_EQ(mock_ptr->invoke_callback(), kFrameCount);
    EXPECT_EQ(consumed, (std::vector<std::uint64_t> { 1, 2, 3, 4, 5, 6 }))
        << "post-restart pulls must continue the playhead (no re-pre-roll, no reset)";

    // 诊断计数累计（不因 restart 清零）；无 reanchor。
    EXPECT_EQ(jb->pull_frames(), 6U * kFrameCount);
    EXPECT_EQ(jb->used_slots(), kPrePushSlots - 6);
    EXPECT_EQ(jb->reanchor_count(), 0U);
    EXPECT_EQ(jb->fill_episodes(), 0U);
    EXPECT_EQ(jb->drop_episodes(), 0U);

    manager.stop();
}

// ---- Test 3：restart underrun recovery（模拟蓝牙切换的 300ms 间隙）----

TEST(PlaybackManagerRestartTest, RestartUnderrunRecovery)
{
    auto jb = make_jitter_buffer();
    ASSERT_NE(jb, nullptr);

    auto mock = std::make_unique<MockAudioPlayback>(MockAudioPlayback::Behavior {
        .threaded = true,
        .start_delay = std::chrono::milliseconds(300),
        .pull_interval = std::chrono::milliseconds(2),
    });
    auto* mock_ptr = mock.get();
    PlaybackManager manager(std::move(mock));

    std::atomic<std::uint64_t> last_seq { 0 };
    const auto started = manager.start(make_playback_config(),
        [&jb, &last_seq](std::span<std::byte> output) noexcept {
            const auto result = jb->pull(output);
            const auto seq = decode_seq(output);
            if (seq != 0) {
                last_seq.store(seq, std::memory_order_release);
            }
            return result.frames_filled;
        });
    ASSERT_TRUE(started.has_value());

    std::atomic<std::uint64_t> producer_seq { kPrePushSlots };
    auto pump_frames = [&](int count) {
        for (int i = 0; i < count; ++i) {
            const auto seq = producer_seq.fetch_add(1, std::memory_order_relaxed) + 1;
            (void)push_frame(*jb, seq);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    };
    std::thread producer([&] { pump_frames(30); });

    // 等真实数据开始流动，然后停止供给，让 JB 完全排空（underrun）。
    ASSERT_TRUE(wait_for([&] { return last_seq.load() > 0; }));
    producer.join();
    ASSERT_TRUE(wait_for([&] { return jb->used_slots() == 0; }, std::chrono::milliseconds(5000)))
        << "jitter buffer did not drain";
    ASSERT_TRUE(wait_for([&] { return jb->pull_silence_frames() > 0; }))
        << "underrun PLC (silence fill) not active";

    // restart：300ms 设备打开间隙，期间 JB 保持排空。
    std::atomic<bool> saw_switching { false };
    std::atomic<bool> sampling { true };
    std::thread sampler([&] {
        while (sampling.load(std::memory_order_acquire)) {
            if (manager.state() == PlaybackState::Switching) {
                saw_switching.store(true, std::memory_order_release);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    });

    const auto restart_result = manager.restart();
    sampling.store(false, std::memory_order_release);
    sampler.join();
    ASSERT_TRUE(restart_result.has_value()) << audio::audio_error_name(restart_result.error());
    EXPECT_TRUE(saw_switching.load()) << "Switching state not observable during restart gap";
    EXPECT_EQ(manager.state(), PlaybackState::Running);
    EXPECT_EQ(mock_ptr->start_calls(), 2U);

    // 新流接上空 JB：PLC/FILL 继续产出静音，无 reanchor、无会话重启。
    const auto silence_before = jb->pull_silence_frames();
    ASSERT_TRUE(wait_for([&] { return jb->pull_silence_frames() > silence_before; }))
        << "new stream did not pull silence (PLC) from empty buffer";
    EXPECT_EQ(jb->reanchor_count(), 0U);
    EXPECT_GE(jb->fill_episodes(), 1U);

    // 恢复供给：从断点序号继续，播放恢复真实数据（不重新建立会话/不重放旧数据）。
    const auto resume_seq = producer_seq.load(std::memory_order_relaxed) + 1;
    std::thread resume([&] { pump_frames(kPrePushSlots + 5); });
    ASSERT_TRUE(wait_for(
        [&] { return last_seq.load(std::memory_order_acquire) >= resume_seq; },
        std::chrono::milliseconds(5000)))
        << "playback did not recover to real data after underrun+restart";
    EXPECT_GE(last_seq.load(), resume_seq);

    resume.join();
    manager.stop();
}

} // namespace
} // namespace aqua::runtime
