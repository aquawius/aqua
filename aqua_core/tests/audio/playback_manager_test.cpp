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
#include "aqua/audio/playback/playback_manager.h"

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

namespace aqua::audio {
namespace {

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

    // 可编排失败：对指定 device 的 start() 返回该错误（nullopt 匹配
    // "跟随系统"候选）。命中规则不进入运行态。
    void fail_device(std::optional<AudioDeviceId> device, AudioError error)
    {
        fail_rules_.emplace_back(device, error);
    }

    // 清空失败规则（模拟设备恢复可用；自动切回测试用）。
    void clear_fail_rules()
    {
        fail_rules_.clear();
    }

    std::expected<void, AudioError> start(const AudioPlaybackConfig& config,
        AudioPlaybackCallback callback,
        AudioPlaybackEventCallback event_callback) noexcept override
    {
        start_attempts_.fetch_add(1, std::memory_order_relaxed);
        start_requests_.push_back(config.device);
        if (running_.load(std::memory_order_acquire)) {
            return std::unexpected(AudioError::AlreadyRunning);
        }
        if (!callback) {
            return std::unexpected(AudioError::InvalidArgument);
        }
        for (const auto& [device, error] : fail_rules_) {
            if (device == config.device) {
                return std::unexpected(error);
            }
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
        // 实际设备回读：请求值即激活设备；nullopt 解析为 mock 默认设备。
        info.device_id = config_.device ? *config_.device : AudioDeviceId("mock-default");
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
    // start() 总进入次数（含被 fail 规则拒绝的尝试），用于候选链断言。
    [[nodiscard]] std::uint64_t start_attempts() const noexcept
    {
        return start_attempts_.load(std::memory_order_relaxed);
    }
    // 每次 start() 的请求 device 序列（含失败尝试），用于候选链顺序断言。
    [[nodiscard]] const std::vector<std::optional<AudioDeviceId>>& start_requests() const noexcept
    {
        return start_requests_;
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
    std::atomic<std::uint64_t> start_attempts_ { 0 };
    // 仅控制线程写（manager 生命周期方法同线程串行），测试断言冷读。
    std::vector<std::pair<std::optional<AudioDeviceId>, AudioError>> fail_rules_;
    std::vector<std::optional<AudioDeviceId>> start_requests_;
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

// ---- A-1：set_playback_device 事务链 ----

using DeviceOpt = std::optional<AudioDeviceId>;

TEST(PlaybackManagerSwitchTest, SetPlaybackDeviceSwitchesToTarget)
{
    auto mock = std::make_unique<MockAudioPlayback>(
        MockAudioPlayback::Behavior { .threaded = false });
    auto* mock_ptr = mock.get();
    PlaybackManager manager(std::move(mock));

    ASSERT_TRUE(manager
                    .start(make_playback_config(),
                        [](std::span<std::byte>) noexcept { return 0U; })
                    .has_value());
    EXPECT_EQ(manager.route_mode(), PlaybackRouteMode::FollowSystem);

    const auto result = manager.set_playback_device(AudioDeviceId("usb-dac"));
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->outcome, SwitchOutcome::Switched);
    EXPECT_EQ(result->last_error, AudioError::None);
    EXPECT_EQ(manager.state(), PlaybackState::Running);
    EXPECT_EQ(manager.route_mode(), PlaybackRouteMode::PreferredDevice);
    // 候选链：[usb-dac]（previous 回读 mock-default，system nullopt——
    // 目标成功后不再尝试）。首个 start 即成功。
    EXPECT_EQ(mock_ptr->start_attempts(), 2U); // 初始 start + 切换 start
    EXPECT_EQ(mock_ptr->start_requests().size(), 2U);
    EXPECT_EQ(mock_ptr->start_requests()[1], DeviceOpt(AudioDeviceId("usb-dac")));
    EXPECT_EQ(manager.stream_info().device_id.value(), "usb-dac");
    // 最近切换结果可回读（诊断源）。
    ASSERT_TRUE(manager.last_switch_result().has_value());
    EXPECT_EQ(manager.last_switch_result()->outcome, SwitchOutcome::Switched);

    manager.stop();
}

TEST(PlaybackManagerSwitchTest, SetPlaybackDeviceRollsBackOnTargetFailure)
{
    auto mock = std::make_unique<MockAudioPlayback>(
        MockAudioPlayback::Behavior { .threaded = false });
    auto* mock_ptr = mock.get();
    // 目标设备 DAC 不支持会话格式（SCO/HFP 16k mono 场景）。
    mock->fail_device(AudioDeviceId("hfp-dac"), AudioError::FormatUnsupported);
    PlaybackManager manager(std::move(mock));

    ASSERT_TRUE(manager
                    .start(make_playback_config(),
                        [](std::span<std::byte>) noexcept { return 0U; })
                    .has_value());

    const auto result = manager.set_playback_device(AudioDeviceId("hfp-dac"));
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->outcome, SwitchOutcome::RolledBack);
    EXPECT_EQ(result->last_error, AudioError::None);
    EXPECT_EQ(manager.state(), PlaybackState::Running);
    // 候选链顺序：[hfp-dac(失败) -> mock-default(回滚成功)]；system 兜底
    // 与 previous 重复？不——previous 是 mock-default，system 是 nullopt，
    // 两者不同但 hfp 失败后回滚成功即止。
    EXPECT_EQ(mock_ptr->start_requests().size(), 3U); // 初始 + 2 次尝试
    EXPECT_EQ(mock_ptr->start_requests()[1], DeviceOpt(AudioDeviceId("hfp-dac")));
    EXPECT_EQ(mock_ptr->start_requests()[2], DeviceOpt(AudioDeviceId("mock-default")));
    // 回滚后仍运行在实际设备上。
    EXPECT_EQ(manager.stream_info().device_id.value(), "mock-default");

    manager.stop();
}

TEST(PlaybackManagerSwitchTest, SetPlaybackDeviceFallsBackToSystem)
{
    auto mock = std::make_unique<MockAudioPlayback>(
        MockAudioPlayback::Behavior { .threaded = false });
    auto* mock_ptr = mock.get();
    // 目标与当前实际设备都失败，只剩系统默认兜底。
    mock->fail_device(AudioDeviceId("dead-usb"), AudioError::DeviceDisconnected);
    mock->fail_device(AudioDeviceId("mock-default"), AudioError::DeviceDisconnected);
    PlaybackManager manager(std::move(mock));

    ASSERT_TRUE(manager
                    .start(make_playback_config(),
                        [](std::span<std::byte>) noexcept { return 0U; })
                    .has_value());

    const auto result = manager.set_playback_device(AudioDeviceId("dead-usb"));
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->outcome, SwitchOutcome::FellBackToSystem);
    EXPECT_EQ(manager.state(), PlaybackState::Running);
    EXPECT_EQ(manager.route_mode(), PlaybackRouteMode::FollowSystem);
    EXPECT_EQ(mock_ptr->start_requests().size(), 4U); // 初始 + 3 次尝试
    EXPECT_EQ(mock_ptr->start_requests()[1], DeviceOpt(AudioDeviceId("dead-usb")));
    EXPECT_EQ(mock_ptr->start_requests()[2], DeviceOpt(AudioDeviceId("mock-default")));
    EXPECT_EQ(mock_ptr->start_requests()[3], std::nullopt);

    manager.stop();
}

TEST(PlaybackManagerSwitchTest, SwitchChainExhaustionIsFatal)
{
    auto mock = std::make_unique<MockAudioPlayback>(
        MockAudioPlayback::Behavior { .threaded = false });
    auto* mock_ptr = mock.get();
    PlaybackManager manager(std::move(mock));

    ASSERT_TRUE(manager
                    .start(make_playback_config(),
                        [](std::span<std::byte>) noexcept { return 0U; })
                    .has_value());

    // 初始流建立后，全部候选（含系统默认）都变得不兼容（模拟 SCO/HFP
    // 接入导致整链 FormatUnsupported）。mock_ptr 在 unique_ptr move 给
    // manager 后依然有效（堆对象地址不变）。
    mock_ptr->fail_device(AudioDeviceId("hfp"), AudioError::FormatUnsupported);
    mock_ptr->fail_device(AudioDeviceId("mock-default"), AudioError::FormatUnsupported);
    mock_ptr->fail_device(std::nullopt, AudioError::FormatUnsupported);

    const auto result = manager.set_playback_device(AudioDeviceId("hfp"));
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), AudioError::FormatUnsupported);
    EXPECT_EQ(manager.state(), PlaybackState::Fatal);
    EXPECT_FALSE(manager.is_running());
    ASSERT_TRUE(manager.last_switch_result().has_value());
    EXPECT_EQ(manager.last_switch_result()->outcome, SwitchOutcome::Fatal);
    EXPECT_EQ(manager.last_switch_result()->last_error, AudioError::FormatUnsupported);

    // Fatal 是终态：后续事务请求被拒绝。
    const auto again = manager.set_playback_device(std::nullopt);
    ASSERT_FALSE(again.has_value());
    EXPECT_EQ(manager.state(), PlaybackState::Fatal);
    // stop() 仍可正常执行（runtime teardown 路径）。
    manager.stop();
    EXPECT_EQ(manager.state(), PlaybackState::Inactive);
}

TEST(PlaybackManagerSwitchTest, CandidateDeduplication)
{
    auto mock = std::make_unique<MockAudioPlayback>(
        MockAudioPlayback::Behavior { .threaded = false });
    auto* mock_ptr = mock.get();
    PlaybackManager manager(std::move(mock));

    AudioPlaybackConfig config = make_playback_config();
    config.device = AudioDeviceId("speaker");
    ASSERT_TRUE(manager
                    .start(config,
                        [](std::span<std::byte>) noexcept { return 0U; })
                    .has_value());

    // target == previous（speaker）：去重后候选链只剩 [speaker]，
    // 一次 start 即成功。
    const auto result = manager.set_playback_device(AudioDeviceId("speaker"));
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->outcome, SwitchOutcome::Switched);
    EXPECT_EQ(mock_ptr->start_requests().size(), 2U); // 初始 + 1 次尝试
    EXPECT_EQ(mock_ptr->start_requests()[1], DeviceOpt(AudioDeviceId("speaker")));

    manager.stop();
}

TEST(PlaybackManagerSwitchTest, ErrorRestartRetryBudget)
{
    auto mock = std::make_unique<MockAudioPlayback>(
        MockAudioPlayback::Behavior { .threaded = false });
    auto* mock_ptr = mock.get();
    PlaybackManager manager(std::move(mock));

    ASSERT_TRUE(manager
                    .start(make_playback_config(),
                        [](std::span<std::byte>) noexcept { return 0U; })
                    .has_value());

    // FollowSystem 模式：错误驱动目标 = nullopt。窗口内前 3 次正常执行。
    for (int i = 0; i < 3; ++i) {
        const auto result = manager.restart_on_error();
        ASSERT_TRUE(result.has_value()) << "restart_on_error #" << (i + 1);
        EXPECT_EQ(result->outcome, SwitchOutcome::Switched);
        EXPECT_EQ(manager.state(), PlaybackState::Running);
    }
    const auto attempts_after_3 = mock_ptr->start_attempts();

    // 第 4 次超限：直接 Fatal，不触碰后端。
    const auto exhausted = manager.restart_on_error();
    ASSERT_FALSE(exhausted.has_value());
    EXPECT_EQ(manager.state(), PlaybackState::Fatal);
    EXPECT_EQ(mock_ptr->start_attempts(), attempts_after_3); // 未发起 start
    ASSERT_TRUE(manager.last_switch_result().has_value());
    EXPECT_EQ(manager.last_switch_result()->outcome, SwitchOutcome::Fatal);
}

TEST(PlaybackManagerSwitchTest, ExplicitSelectionResetsRetryBudget)
{
    auto mock = std::make_unique<MockAudioPlayback>(
        MockAudioPlayback::Behavior { .threaded = false });
    PlaybackManager manager(std::move(mock));

    ASSERT_TRUE(manager
                    .start(make_playback_config(),
                        [](std::span<std::byte>) noexcept { return 0U; })
                    .has_value());

    // 消耗 2 次错误驱动预算（窗口内还剩 1 次）。
    for (int i = 0; i < 2; ++i) {
        ASSERT_TRUE(manager.restart_on_error().has_value());
    }

    // 用户显式选择：成功且重置窗口。
    const auto selected = manager.set_playback_device(AudioDeviceId("usb-dac"));
    ASSERT_TRUE(selected.has_value());
    EXPECT_EQ(selected->outcome, SwitchOutcome::Switched);

    // 窗口已重置：错误驱动预算恢复为满额 3 次。
    for (int i = 0; i < 3; ++i) {
        const auto result = manager.restart_on_error();
        ASSERT_TRUE(result.has_value()) << "restart_on_error after reset #" << (i + 1);
    }
    // 第 4 次再次超限（证明重置后计数从零开始）。
    ASSERT_FALSE(manager.restart_on_error().has_value());
    EXPECT_EQ(manager.state(), PlaybackState::Fatal);
}

// ---- PreferCurrent 起步（"自动切换播放设备"关）----

TEST(PlaybackManagerSwitchTest, PreferCurrentPinsFirstStreamDevice)
{
    auto mock = std::make_unique<MockAudioPlayback>(
        MockAudioPlayback::Behavior { .threaded = false });
    auto* mock_ptr = mock.get();
    PlaybackManager manager(std::move(mock));

    // "自动切换"关：首流（无显式设备）成功后钉住实际设备。
    manager.set_prefer_current_on_start(true);
    ASSERT_TRUE(manager
                    .start(make_playback_config(),
                        [](std::span<std::byte>) noexcept { return 0U; })
                    .has_value());
    EXPECT_EQ(manager.route_mode(), PlaybackRouteMode::PreferCurrent);
    // 钉住值 = stream_info 回读的实际设备（mock 的 nullopt 解析结果）。
    ASSERT_TRUE(manager.requested_device().has_value());
    EXPECT_EQ(manager.requested_device()->value(), "mock-default");

    // 错误驱动 restart 锚定钉住设备，不跟随系统默认：即使系统默认候选
    // （nullopt）不可用，restart 仍在钉住设备上成功（nullopt 未被尝试）。
    mock_ptr->fail_device(std::nullopt, AudioError::DeviceDisconnected);
    const auto result = manager.restart_on_error();
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->outcome, SwitchOutcome::Switched);
    EXPECT_EQ(manager.state(), PlaybackState::Running);
    EXPECT_EQ(mock_ptr->start_requests().size(), 2U); // 初始 nullopt + 钉住设备
    EXPECT_EQ(mock_ptr->start_requests()[1], DeviceOpt(AudioDeviceId("mock-default")));
    // 路由模式不变：fallback 是临时降级，用户意图（保持当前设备）不动。
    EXPECT_EQ(manager.route_mode(), PlaybackRouteMode::PreferCurrent);

    manager.stop();
}

TEST(PlaybackManagerSwitchTest, PreferCurrentFallsBackToSystemWhenPinnedDeviceDies)
{
    auto mock = std::make_unique<MockAudioPlayback>(
        MockAudioPlayback::Behavior { .threaded = false });
    auto* mock_ptr = mock.get();
    PlaybackManager manager(std::move(mock));

    manager.set_prefer_current_on_start(true);
    ASSERT_TRUE(manager
                    .start(make_playback_config(),
                        [](std::span<std::byte>) noexcept { return 0U; })
                    .has_value());

    // 钉住设备被拔：候选链 [pinned(失败) -> previous(=pinned 去重) ->
    // system_default(成功)]，降级为系统默认。
    mock_ptr->fail_device(AudioDeviceId("mock-default"), AudioError::DeviceDisconnected);
    const auto result = manager.restart_on_error();
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->outcome, SwitchOutcome::FellBackToSystem);
    EXPECT_EQ(manager.state(), PlaybackState::Running);
    EXPECT_EQ(mock_ptr->start_requests().size(), 3U); // 初始 + pinned 失败 + 系统
    EXPECT_EQ(mock_ptr->start_requests()[1], DeviceOpt(AudioDeviceId("mock-default")));
    EXPECT_EQ(mock_ptr->start_requests()[2], std::nullopt);

    manager.stop();
}

// ---- 设备集合推送（on_devices_changed，playback_switching_design.md §5 rev2）----

TEST(PlaybackManagerDeviceEventTest, FirstSnapshotIsBaselineOnly)
{
    auto mock = std::make_unique<MockAudioPlayback>(
        MockAudioPlayback::Behavior { .threaded = false });
    auto* mock_ptr = mock.get();
    PlaybackManager manager(std::move(mock));

    ASSERT_TRUE(manager
                    .start(make_playback_config(),
                        [](std::span<std::byte>) noexcept { return 0U; })
                    .has_value());
    EXPECT_EQ(manager.route_mode(), PlaybackRouteMode::FollowSystem);

    // 首份快照只作基线：即使包含"新"设备也不触发跟随（连接初期的初始
    // 列表不是新增）。
    EXPECT_FALSE(manager.on_devices_changed({ AudioDeviceId("bt-headset") }));
    EXPECT_EQ(mock_ptr->start_attempts(), 1U); // 只有初始 start

    manager.stop();
}

TEST(PlaybackManagerDeviceEventTest, FollowSystemFollowsNewDevice)
{
    auto mock = std::make_unique<MockAudioPlayback>(
        MockAudioPlayback::Behavior { .threaded = false });
    auto* mock_ptr = mock.get();
    PlaybackManager manager(std::move(mock));

    ASSERT_TRUE(manager
                    .start(make_playback_config(),
                        [](std::span<std::byte>) noexcept { return 0U; })
                    .has_value());
    ASSERT_FALSE(manager.on_devices_changed({ AudioDeviceId("mock-default") })); // 基线

    // 新增可切换设备：FollowSystem 重开流跟随系统默认（target=nullopt）。
    EXPECT_TRUE(manager.on_devices_changed(
        { AudioDeviceId("mock-default"), AudioDeviceId("bt-headset") }));
    EXPECT_EQ(manager.state(), PlaybackState::Running);
    EXPECT_EQ(mock_ptr->start_attempts(), 2U);
    EXPECT_EQ(mock_ptr->start_requests()[1], std::nullopt);
    // 路由模式不变。
    EXPECT_EQ(manager.route_mode(), PlaybackRouteMode::FollowSystem);

    // 同集合再次推送：无新增，不动作。
    EXPECT_FALSE(manager.on_devices_changed(
        { AudioDeviceId("mock-default"), AudioDeviceId("bt-headset") }));
    EXPECT_EQ(mock_ptr->start_attempts(), 2U);

    manager.stop();
}

TEST(PlaybackManagerDeviceEventTest, ActiveDeviceGoneTriggersEagerRestart)
{
    auto mock = std::make_unique<MockAudioPlayback>(
        MockAudioPlayback::Behavior { .threaded = false });
    auto* mock_ptr = mock.get();
    PlaybackManager manager(std::move(mock));

    ASSERT_TRUE(manager
                    .start(make_playback_config(),
                        [](std::span<std::byte>) noexcept { return 0U; })
                    .has_value());
    ASSERT_FALSE(manager.on_devices_changed({ AudioDeviceId("mock-default") })); // 基线

    // 活跃设备消失：eager restart（FollowSystem 目标 = 系统默认）。
    EXPECT_TRUE(manager.on_devices_changed({ AudioDeviceId("usb-dac") }));
    EXPECT_EQ(manager.state(), PlaybackState::Running);
    EXPECT_EQ(mock_ptr->start_attempts(), 2U);
    EXPECT_EQ(mock_ptr->start_requests()[1], std::nullopt);

    manager.stop();
}

TEST(PlaybackManagerDeviceEventTest, PreferredDeviceStickyIntentSurvivesFallback)
{
    auto mock = std::make_unique<MockAudioPlayback>(
        MockAudioPlayback::Behavior { .threaded = false });
    auto* mock_ptr = mock.get();
    PlaybackManager manager(std::move(mock));

    AudioPlaybackConfig config = make_playback_config();
    config.device = AudioDeviceId("dac");
    ASSERT_TRUE(manager
                    .start(config, [](std::span<std::byte>) noexcept { return 0U; })
                    .has_value());
    EXPECT_EQ(manager.route_mode(), PlaybackRouteMode::PreferredDevice);
    ASSERT_FALSE(manager.on_devices_changed({ AudioDeviceId("dac") })); // 基线

    // 钉住设备被拔：eager restart 目标 = sticky 意图 "dac"（失败）→
    // 链兜底落系统默认；route mode 与请求设备（用户意图）不丢。
    mock_ptr->fail_device(AudioDeviceId("dac"), AudioError::DeviceDisconnected);
    EXPECT_TRUE(manager.on_devices_changed({ AudioDeviceId("speaker") }));
    EXPECT_EQ(manager.state(), PlaybackState::Running);
    EXPECT_EQ(mock_ptr->start_requests().size(), 3U); // 初始 + dac 失败 + 系统兜底
    EXPECT_EQ(mock_ptr->start_requests()[1], DeviceOpt(AudioDeviceId("dac")));
    EXPECT_EQ(mock_ptr->start_requests()[2], std::nullopt);
    EXPECT_EQ(manager.route_mode(), PlaybackRouteMode::PreferredDevice);
    ASSERT_TRUE(manager.requested_device().has_value());
    EXPECT_EQ(manager.requested_device()->value(), "dac"); // sticky 意图保留
    EXPECT_EQ(manager.stream_info().device_id.value(), "mock-default"); // 实际在系统默认

    // 再次错误驱动 restart：目标仍是 sticky "dac"（而非当前的系统默认）。
    (void)manager.restart_on_error();
    EXPECT_EQ(mock_ptr->start_requests()[3], DeviceOpt(AudioDeviceId("dac")));

    manager.stop();
}

TEST(PlaybackManagerDeviceEventTest, PreferredDeviceAutoSwitchBackOnReappear)
{
    auto mock = std::make_unique<MockAudioPlayback>(
        MockAudioPlayback::Behavior { .threaded = false });
    auto* mock_ptr = mock.get();
    PlaybackManager manager(std::move(mock));

    AudioPlaybackConfig config = make_playback_config();
    config.device = AudioDeviceId("dac");
    ASSERT_TRUE(manager
                    .start(config, [](std::span<std::byte>) noexcept { return 0U; })
                    .has_value());
    ASSERT_FALSE(manager.on_devices_changed({ AudioDeviceId("dac") })); // 基线

    // 钉住设备被拔 → 回退系统默认（用户意图保留）。
    mock_ptr->fail_device(AudioDeviceId("dac"), AudioError::DeviceDisconnected);
    ASSERT_TRUE(manager.on_devices_changed(
        { AudioDeviceId("speaker"), AudioDeviceId("mock-default") }));
    EXPECT_EQ(manager.stream_info().device_id.value(), "mock-default");

    // 设备回归（且恢复可用；当前兜底设备仍在列表中，不触发 eager
    // restart）：走自动切回分支，route mode 保持 PreferredDevice。
    mock_ptr->clear_fail_rules();
    EXPECT_TRUE(manager.on_devices_changed(
        { AudioDeviceId("speaker"), AudioDeviceId("mock-default"), AudioDeviceId("dac") }));
    EXPECT_EQ(manager.state(), PlaybackState::Running);
    EXPECT_EQ(manager.stream_info().device_id.value(), "dac");
    EXPECT_EQ(manager.route_mode(), PlaybackRouteMode::PreferredDevice);
    // 切回成功是有用户意义的 Switched（诊断横幅据此提示）。
    ASSERT_TRUE(manager.last_switch_result().has_value());
    EXPECT_EQ(manager.last_switch_result()->outcome, SwitchOutcome::Switched);

    // 已在钉住设备上：再次推送同集合不动作。
    EXPECT_FALSE(manager.on_devices_changed(
        { AudioDeviceId("speaker"), AudioDeviceId("mock-default"), AudioDeviceId("dac") }));

    manager.stop();
}

TEST(PlaybackManagerDeviceEventTest, PreferCurrentIgnoresDeviceSetChanges)
{
    auto mock = std::make_unique<MockAudioPlayback>(
        MockAudioPlayback::Behavior { .threaded = false });
    auto* mock_ptr = mock.get();
    PlaybackManager manager(std::move(mock));

    manager.set_prefer_current_on_start(true);
    ASSERT_TRUE(manager
                    .start(make_playback_config(),
                        [](std::span<std::byte>) noexcept { return 0U; })
                    .has_value());
    ASSERT_EQ(manager.route_mode(), PlaybackRouteMode::PreferCurrent);
    ASSERT_FALSE(manager.on_devices_changed({ AudioDeviceId("mock-default") })); // 基线

    // 新设备接入：PreferCurrent 不跟随（钉住首流实际设备）。
    EXPECT_FALSE(manager.on_devices_changed(
        { AudioDeviceId("mock-default"), AudioDeviceId("bt-headset") }));
    EXPECT_EQ(mock_ptr->start_attempts(), 1U);
    EXPECT_EQ(manager.route_mode(), PlaybackRouteMode::PreferCurrent);

    manager.stop();
}

} // namespace
} // namespace aqua::audio
