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
#include "aqua/audio/devices/audio_device_manager.h"
#include "aqua/audio/playback/audio_playback.h"
#include "aqua/audio/playback/playback_manager.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
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
            transient_rules_.clear();
        }

        // 编排"瞬时"失败：对该 device 的 start() 先失败 times 次，之后放行。
        // 模拟移动端 break-before-make 之后重开刚关闭设备的异步摘除窗口。
        void fail_device_times(
            std::optional<AudioDeviceId> device, AudioError error, int times)
        {
            transient_rules_.push_back(TransientRule { device, error, times });
        }

        std::expected<void, AudioError> start(const AudioPlaybackConfig& config,
            AudioPlaybackCallback callback,
            AudioPlaybackEventCallback event_callback) noexcept override
        {
            start_attempts_.fetch_add(1, std::memory_order_relaxed);
            // 路由请求回读（旧 config.device 语义）：nullopt = "跟随系统"候选。
            const auto requested_device = config.route.endpoint_request();
            start_requests_.push_back(requested_device);
            if (running_.load(std::memory_order_acquire)) {
                return std::unexpected(AudioError::AlreadyRunning);
            }
            if (!callback) {
                return std::unexpected(AudioError::InvalidArgument);
            }
            for (auto& rule : transient_rules_) {
                if (rule.device == requested_device && rule.remaining > 0) {
                    --rule.remaining;
                    return std::unexpected(rule.error);
                }
            }
            for (const auto& [device, error] : fail_rules_) {
                if (device == requested_device) {
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
            // 认领新流：generation 每次成功 start 递增（从 1 起，0 保留给
            // 「无流」），manager 的 provenance 过滤据此判别事件归属。
            // 世代号取自单调计数器：stop() 会把 generation_ 归零，若共用一个
            // 原子则下一轮 start() 会重号，旧流的迟到事件就能冒充新流
            // （与生产 backend 同一约束）。
            generation_.store(
                generation_counter_.fetch_add(1, std::memory_order_acq_rel) + 1,
                std::memory_order_release);
            running_.store(true, std::memory_order_release);
            if (behavior_.threaded) {
                thread_ = std::jthread(&MockAudioPlayback::thread_main, this);
            }
            return { };
        }

        bool is_running() const noexcept override
        {
            return running_.load(std::memory_order_acquire);
        }

        [[nodiscard]] StreamGeneration generation() const noexcept override
        {
            return generation_.load(std::memory_order_acquire);
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
            const auto requested = config_.route.endpoint_request();
            info.device_id = requested ? *requested : system_default_;
            return info;
        }

        // "跟随系统"候选回读到的设备：模拟平台把 nullopt 解析成**当前**默认
        // 端点。tick 的默认设备跟随路径依赖它与 StubDeviceManager 的默认值一致，
        // 否则回读永远停在旧默认，tick 会每轮都判定"默认又变了"。
        void set_system_default_device(AudioDeviceId id)
        {
            system_default_ = std::move(id);
        }

        void stop() noexcept override
        {
            stop_flag_.store(true, std::memory_order_release);
            if (thread_.joinable() && thread_.get_id() != std::this_thread::get_id()) {
                thread_.join();
            }
            const bool was_running = running_.exchange(false, std::memory_order_acq_rel);
            // 撤销认领：归零后 manager 的 provenance 过滤丢弃本世代的迟到事件。
            generation_.store(kNoStreamGeneration, std::memory_order_release);
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

        // 模拟 backend 侧的运行期致命错误（对称 AAudioAudioPlayback::
        // report_fatal_once）：把流标记为已死，并带**当前世代**投递事件。
        // 两步都必须有——PlaybackManager 的 settle 分类以 is_running()==false
        // 作为"这条流真的死过"的可观测判据。
        void simulate_stream_death(AudioError error) noexcept
        {
            running_.store(false, std::memory_order_release);
            if (event_callback_) {
                event_callback_(AudioStreamEvent { error,
                    generation_.load(std::memory_order_acquire) });
            }
        }

        // 以**指定世代**投递事件（不改变 running_）：直接验证 manager 的
        // provenance 过滤——旧世代的迟到讣告必须被丢弃且不触碰任何状态。
        void fire_event_with_generation(AudioError error,
            StreamGeneration generation) noexcept
        {
            if (event_callback_) {
                event_callback_(AudioStreamEvent { error, generation });
            }
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
        std::jthread thread_;
        std::atomic<bool> running_ { false };
        // 当前流代号（每次成功 start 递增；stop 归零；见 audio_stream_event.h）。
        std::atomic<StreamGeneration> generation_counter_ { kNoStreamGeneration };
        std::atomic<StreamGeneration> generation_ { kNoStreamGeneration };
        std::atomic<bool> stop_flag_ { false };
        std::atomic<int> concurrent_ { 0 };
        std::atomic<int> max_concurrent_ { 0 };
        std::atomic<std::uint64_t> start_calls_ { 0 };
        std::atomic<std::uint64_t> start_attempts_ { 0 };
        // 仅控制线程写（manager 生命周期方法同线程串行），测试断言冷读。
        // 瞬时失败规则（带剩余次数，用于重开刚关闭设备的重试测试）。
        struct TransientRule {
            std::optional<AudioDeviceId> device;
            AudioError error;
            int remaining;
        };
        std::vector<TransientRule> transient_rules_;
        std::vector<std::pair<std::optional<AudioDeviceId>, AudioError>> fail_rules_;
        std::vector<std::optional<AudioDeviceId>> start_requests_;
        std::atomic<std::uint64_t> stop_calls_ { 0 };
        // nullopt（跟随系统）候选回读到的设备；默认值保持既有测试语义不变。
        AudioDeviceId system_default_ = AudioDeviceId("mock-default");
    };

    AudioPlaybackConfig make_playback_config(
        std::optional<AudioDeviceId> pin = std::nullopt)
    {
        AudioPlaybackConfig config;
        config.format = make_format();
        config.frames_per_buffer = kFrameCount;
        config.route = AudioRoute::from_optional(std::move(pin));
        return config;
    }

    // 最小设备系统入口 stub：只服务 tick() 的两步轮询（设备集合 + 默认设备）。
    // 存在的理由是让 tick 驱动的轮询路径可测——此前 PlaybackManager 的注入式
    // 构造不接受设备入口，Windows 上"钉住设备回归自动切回"这条路径无从验证。
    class StubDeviceManager final : public AudioDeviceManager {
    public:
        void set_output(std::vector<std::string> ids)
        {
            output_.clear();
            for (auto& id : ids) {
                AudioDevice device;
                device.id = AudioDeviceId(std::move(id));
                device.direction = AudioDeviceDirection::OUTPUT;
                output_.push_back(std::move(device));
            }
        }

        void set_default_output(std::string id) { default_output_ = AudioDeviceId(std::move(id)); }

        [[nodiscard]] std::vector<AudioDevice>
        enumerate(AudioDeviceDirection direction) const override
        {
            return direction == AudioDeviceDirection::OUTPUT ? output_ : std::vector<AudioDevice> { };
        }

        [[nodiscard]] std::optional<AudioDevice>
        default_device(AudioDeviceDirection direction) const override
        {
            if (direction != AudioDeviceDirection::OUTPUT) {
                return std::nullopt;
            }
            for (const auto& device : output_) {
                if (device.id == default_output_) {
                    return device;
                }
            }
            return std::nullopt;
        }

        [[nodiscard]] std::expected<AudioFormat, AudioError>
        default_format(AudioDeviceDirection,
            const std::optional<AudioDeviceId>&) const override
        {
            return make_format();
        }

        [[nodiscard]] std::expected<AudioDevice, AudioError>
        resolve(AudioDeviceDirection direction,
            const std::optional<AudioDeviceId>& requested) const override
        {
            if (!requested) {
                const auto fallback = default_device(direction);
                if (!fallback) {
                    return std::unexpected(AudioError::DeviceNotFound);
                }
                return *fallback;
            }
            for (const auto& device : enumerate(direction)) {
                if (device.id == *requested) {
                    return device;
                }
            }
            return std::unexpected(AudioError::DeviceNotFound);
        }

    private:
        std::vector<AudioDevice> output_;
        AudioDeviceId default_output_;
    };

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
        std::jthread producer([&]() {
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
        std::jthread producer([&] { pump_frames(30); });

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
        std::jthread sampler([&] {
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
        std::jthread resume([&] { pump_frames(kPrePushSlots + 5); });
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
        EXPECT_EQ(manager.intent_owner(), RouteIntentOwner::None);

        const auto result = manager.set_playback_device(AudioDeviceId("usb-dac"));
        ASSERT_TRUE(result.has_value());
        EXPECT_EQ(result->outcome, SwitchOutcome::Switched);
        EXPECT_EQ(result->last_error, AudioError::None);
        EXPECT_EQ(manager.state(), PlaybackState::Running);
        EXPECT_EQ(manager.intent_owner(), RouteIntentOwner::User);
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
        // 显式选择的 pin 不因 fallback 降级而丢失：路由仍是 PreferredDevice，
        // sticky 意图保留，设备回归可自动切回（与错误驱动 fallback 对称）。
        EXPECT_EQ(manager.intent_owner(), RouteIntentOwner::User);
        ASSERT_TRUE(manager.preferred_or_active_device().has_value());
        EXPECT_EQ(manager.preferred_or_active_device()->value(), "dead-usb");
        EXPECT_EQ(mock_ptr->start_requests().size(), 4U); // 初始 + 3 次尝试
        EXPECT_EQ(mock_ptr->start_requests()[1], DeviceOpt(AudioDeviceId("dead-usb")));
        EXPECT_EQ(mock_ptr->start_requests()[2], DeviceOpt(AudioDeviceId("mock-default")));
        EXPECT_EQ(mock_ptr->start_requests()[3], std::nullopt);

        // 设备恢复后经快照回归：自动切回钉住设备（无需用户再选一次）。
        mock_ptr->clear_fail_rules();
        ASSERT_FALSE(manager.on_devices_changed({ AudioDeviceId("dead-usb") })); // 基线重建
        EXPECT_TRUE(manager.on_devices_changed(
            { AudioDeviceId("mock-default"), AudioDeviceId("dead-usb") }));
        EXPECT_EQ(manager.stream_info().device_id.value(), "dead-usb");
        EXPECT_EQ(manager.intent_owner(), RouteIntentOwner::User);

        manager.stop();
    }

    // 回归：显式换设备时，若只有"刚关闭的上一设备"是瞬时不可用（移动端 A2DP /
    // USB 摘除是异步的），必须靠有界重试回到上一设备保住会话，而不是让候选链
    // 耗尽的 Fatal 把整条连接带崩（用户观感：换个设备结果断线）。
    TEST(PlaybackManagerSwitchTest, ExplicitSwitchRetriesPreviousBeforeFatal)
    {
        auto mock = std::make_unique<MockAudioPlayback>(
            MockAudioPlayback::Behavior { .threaded = false });
        auto* mock_ptr = mock.get();
        PlaybackManager manager(std::move(mock));

        ASSERT_TRUE(manager
                .start(make_playback_config(),
                    [](std::span<std::byte>) noexcept { return 0U; })
                .has_value());

        // 目标设备与系统默认"永久"不可用；上一设备（mock-default）瞬时不可用
        // 一次后恢复。链式尝试全灭后，重试应当救回会话。
        mock_ptr->fail_device(AudioDeviceId("speaker-new"), AudioError::DeviceDisconnected);
        mock_ptr->fail_device(std::nullopt, AudioError::DeviceDisconnected);
        mock_ptr->fail_device_times(
            AudioDeviceId("mock-default"), AudioError::DeviceDisconnected, 1);

        const auto result = manager.set_playback_device(AudioDeviceId("speaker-new"));
        ASSERT_TRUE(result.has_value());
        EXPECT_EQ(result->outcome, SwitchOutcome::RolledBack);
        EXPECT_EQ(manager.state(), PlaybackState::Running);
        EXPECT_TRUE(manager.is_running());
        // 初始 + 目标 + previous(瞬时失败) + nullopt + previous(重试成功)
        ASSERT_EQ(mock_ptr->start_requests().size(), 5U);
        EXPECT_EQ(mock_ptr->start_requests()[1], DeviceOpt(AudioDeviceId("speaker-new")));
        EXPECT_EQ(mock_ptr->start_requests()[2], DeviceOpt(AudioDeviceId("mock-default")));
        EXPECT_EQ(mock_ptr->start_requests()[3], std::nullopt);
        EXPECT_EQ(mock_ptr->start_requests()[4], DeviceOpt(AudioDeviceId("mock-default")));
        // 回滚不算切换成功：sticky 意图仍是用户选的设备（设备回归可自动切回）。
        EXPECT_EQ(manager.intent_owner(), RouteIntentOwner::User);

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

    TEST(PlaybackManagerSwitchTest, RestartRejectedInFatalState)
    {
        auto mock = std::make_unique<MockAudioPlayback>(
            MockAudioPlayback::Behavior { .threaded = false });
        auto* mock_ptr = mock.get();
        PlaybackManager manager(std::move(mock));

        ASSERT_TRUE(manager
                .start(make_playback_config(),
                    [](std::span<std::byte>) noexcept { return 0U; })
                .has_value());

        mock_ptr->fail_device(AudioDeviceId("hfp"), AudioError::FormatUnsupported);
        mock_ptr->fail_device(AudioDeviceId("mock-default"), AudioError::FormatUnsupported);
        mock_ptr->fail_device(std::nullopt, AudioError::FormatUnsupported);

        const auto result = manager.set_playback_device(AudioDeviceId("hfp"));
        ASSERT_FALSE(result.has_value());
        ASSERT_EQ(manager.state(), PlaybackState::Fatal);

        // restart() 同样守卫 Fatal：不得把终态降级回 Inactive。
        const auto restarted = manager.restart();
        ASSERT_FALSE(restarted.has_value());
        EXPECT_EQ(manager.state(), PlaybackState::Fatal);

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
        config.route = AudioRoute::pin(AudioDeviceId("speaker"));
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

    TEST(PlaybackManagerSwitchTest, FollowSystemErrorTriesCurrentDefaultOnly)
    {
        auto mock = std::make_unique<MockAudioPlayback>(
            MockAudioPlayback::Behavior { .threaded = false });
        auto* mock_ptr = mock.get();
        PlaybackManager manager(std::move(mock));

        ASSERT_TRUE(manager
                .start(make_playback_config(),
                    [](std::span<std::byte>) noexcept { return 0U; })
                .has_value());

        // 自动跟随单候选直达当前默认：nullopt 失败即按链耗尽 Fatal，
        // 不回滚 previous（跟随语义下旧设备正是要离开的）。
        mock_ptr->fail_device(std::nullopt, AudioError::DeviceDisconnected);
        const auto result = manager.restart_on_error();
        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(manager.state(), PlaybackState::Fatal);
        // 初始 nullopt + 本次 nullopt：previous（mock-default）未被尝试。
        ASSERT_EQ(mock_ptr->start_requests().size(), 2U);
        EXPECT_EQ(mock_ptr->start_requests()[1], std::nullopt);

        manager.stop();
    }

    TEST(PlaybackManagerSwitchTest, AutoFollowConsumesSharedBudget)
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

        // 快照驱动的自动跟随同样消费重试预算（与错误驱动共享窗口）：
        // 3 次成功跟随，第 4 次超限 Fatal 且不触碰后端。
        for (int i = 0; i < 3; ++i) {
            char id[32];
            std::snprintf(id, sizeof(id), "new-device-%d", i);
            ASSERT_TRUE(manager.on_devices_changed(
                { AudioDeviceId("mock-default"), AudioDeviceId(id) }))
                << "follow #" << (i + 1);
            EXPECT_EQ(manager.state(), PlaybackState::Running);
        }
        const auto attempts = mock_ptr->start_attempts();
        // 第 4 次：预算耗尽拒绝（事件仍消费，返回 true），不触碰后端。
        EXPECT_TRUE(manager.on_devices_changed(
            { AudioDeviceId("mock-default"), AudioDeviceId("new-device-99") }));
        EXPECT_EQ(manager.state(), PlaybackState::Fatal);
        EXPECT_EQ(mock_ptr->start_attempts(), attempts); // 预算拒绝，未发起 start

        manager.stop();
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
        EXPECT_EQ(manager.intent_owner(), RouteIntentOwner::Application);
        // 钉住值 = stream_info 回读的实际设备（mock 的 nullopt 解析结果）。
        ASSERT_TRUE(manager.preferred_or_active_device().has_value());
        EXPECT_EQ(manager.preferred_or_active_device()->value(), "mock-default");

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
        EXPECT_EQ(manager.intent_owner(), RouteIntentOwner::Application);

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
        EXPECT_EQ(manager.intent_owner(), RouteIntentOwner::None);

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
        EXPECT_EQ(manager.intent_owner(), RouteIntentOwner::None);

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
        config.route = AudioRoute::pin(AudioDeviceId("dac"));
        ASSERT_TRUE(manager
                .start(config, [](std::span<std::byte>) noexcept { return 0U; })
                .has_value());
        EXPECT_EQ(manager.intent_owner(), RouteIntentOwner::User);
        ASSERT_FALSE(manager.on_devices_changed({ AudioDeviceId("dac") })); // 基线

        // 钉住设备被拔：eager restart 目标 = sticky 意图 "dac"（失败）→
        // 链兜底落系统默认；route mode 与请求设备（用户意图）不丢。
        mock_ptr->fail_device(AudioDeviceId("dac"), AudioError::DeviceDisconnected);
        EXPECT_TRUE(manager.on_devices_changed({ AudioDeviceId("speaker") }));
        EXPECT_EQ(manager.state(), PlaybackState::Running);
        EXPECT_EQ(mock_ptr->start_requests().size(), 3U); // 初始 + dac 失败 + 系统兜底
        EXPECT_EQ(mock_ptr->start_requests()[1], DeviceOpt(AudioDeviceId("dac")));
        EXPECT_EQ(mock_ptr->start_requests()[2], std::nullopt);
        EXPECT_EQ(manager.intent_owner(), RouteIntentOwner::User);
        ASSERT_TRUE(manager.preferred_or_active_device().has_value());
        EXPECT_EQ(manager.preferred_or_active_device()->value(), "dac"); // sticky 意图保留
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
        config.route = AudioRoute::pin(AudioDeviceId("dac"));
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
        EXPECT_EQ(manager.intent_owner(), RouteIntentOwner::User);
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
        ASSERT_EQ(manager.intent_owner(), RouteIntentOwner::Application);
        ASSERT_FALSE(manager.on_devices_changed({ AudioDeviceId("mock-default") })); // 基线

        // 新设备接入：PreferCurrent 不跟随（钉住首流实际设备）。
        EXPECT_FALSE(manager.on_devices_changed(
            { AudioDeviceId("mock-default"), AudioDeviceId("bt-headset") }));
        EXPECT_EQ(mock_ptr->start_attempts(), 1U);
        EXPECT_EQ(manager.intent_owner(), RouteIntentOwner::Application);

        manager.stop();
    }

    // ---- 路由未稳定（settle）预算：Android 蓝牙 -> 扬声器断连的回归守护 ----
    //
    // 实测依据 temp/android_switch.log：切到内建扬声器后每条新流在
    // requestStart 成功后 6~8ms 收到 AAUDIO_ERROR_DISCONNECTED，四次尝试全挤
    // 在 256ms 内，把 10s/3 的设备丢失预算烧光 -> Fatal -> supervision 停掉整
    // 个 ClientRuntime（用户观感「换个设备把连接搞断了」）。同一日志里 4.6s /
    // 614ms 无 close 的自发 DISCONNECTED 证明这些事件属于**当前**流，不是旧流
    // 的迟到讣告——所以修法是给平台路由留时间，而不是过滤事件归属。

    TEST(PlaybackManagerSettleTest, ImmediateStreamDeathUsesSettleBudgetNotDeviceLossBudget)
    {
        auto mock = std::make_unique<MockAudioPlayback>(
            MockAudioPlayback::Behavior { .threaded = false });
        auto* mock_ptr = mock.get();
        PlaybackManager manager(std::move(mock));

        auto config = make_playback_config();
        config.route = AudioRoute::pin(AudioDeviceId("dac"));
        ASSERT_TRUE(manager
                .start(config, [](std::span<std::byte>) noexcept { return 0U; })
                .has_value());
        ASSERT_EQ(manager.intent_owner(), RouteIntentOwner::User);

        // 连续 4 次「刚起来就死」。旧行为：第 4 次耗尽 10s/3 的设备丢失预算
        // -> Fatal。新行为：全部走独立的 settle 预算，会话保住。
        // 每次间隔 > kSettleRetryInterval(200ms) 以躲过节流（节流本身由下一个
        // 测试覆盖）。
        for (int i = 0; i < 4; ++i) {
            mock_ptr->simulate_stream_death(AudioError::DeviceDisconnected);
            const auto result = manager.restart_on_error();
            ASSERT_TRUE(result.has_value())
                << "settle restart #" << (i + 1) << " should have recovered, got "
                << audio_error_name(result.error());
            ASSERT_EQ(manager.state(), PlaybackState::Running)
                << "settle restart #" << (i + 1);
            std::this_thread::sleep_for(std::chrono::milliseconds(210));
        }
        EXPECT_NE(manager.state(), PlaybackState::Fatal);

        // 设备丢失预算必须**完好无损**：让流活过 settle 窗口后再死，仍应能连
        // 续恢复 3 次，第 4 次才 Fatal。若 settle 失败误占了设备丢失预算，第
        // 一次就会直接 Fatal。
        for (int i = 0; i < 3; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(410));
            mock_ptr->simulate_stream_death(AudioError::DeviceDisconnected);
            const auto result = manager.restart_on_error();
            ASSERT_TRUE(result.has_value())
                << "device-loss restart #" << (i + 1) << " should have recovered";
            ASSERT_EQ(manager.state(), PlaybackState::Running);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(410));
        mock_ptr->simulate_stream_death(AudioError::DeviceDisconnected);
        EXPECT_FALSE(manager.restart_on_error().has_value());
        EXPECT_EQ(manager.state(), PlaybackState::Fatal);

        manager.stop();
    }

    // 节流的语义是「推迟」而不是「升级」：命中节流时不得改动任何状态，让
    // ClientRuntime 的 supervision tick（500ms）经 silent-death 兜底分支再驱动
    // 一次。这就是 pacing 的来源——不在 ioc 线程上 sleep，也不会把一次路由抖
    // 动升级成 Fatal。
    TEST(PlaybackManagerSettleTest, SettleRestartIsThrottledWithoutEscalatingToFatal)
    {
        auto mock = std::make_unique<MockAudioPlayback>(
            MockAudioPlayback::Behavior { .threaded = false });
        auto* mock_ptr = mock.get();
        PlaybackManager manager(std::move(mock));

        auto config = make_playback_config();
        config.route = AudioRoute::pin(AudioDeviceId("dac"));
        ASSERT_TRUE(manager
                .start(config, [](std::span<std::byte>) noexcept { return 0U; })
                .has_value());

        mock_ptr->simulate_stream_death(AudioError::DeviceDisconnected);
        ASSERT_TRUE(manager.restart_on_error().has_value());
        const auto attempts_after_first = mock_ptr->start_attempts();

        // 立刻再死一次：落在 kSettleRetryInterval 内 -> 节流。
        mock_ptr->simulate_stream_death(AudioError::DeviceDisconnected);
        const auto throttled = manager.restart_on_error();
        EXPECT_FALSE(throttled.has_value());
        // 关键：节流**不升级**。状态原样保留，supervision tick 因此仍会经
        // silent-death 分支（Running && !is_running）再次驱动恢复。
        EXPECT_EQ(manager.state(), PlaybackState::Running);
        EXPECT_FALSE(manager.is_running());
        EXPECT_EQ(mock_ptr->start_attempts(), attempts_after_first); // 未触事务

        // 过了节流间隔就能继续恢复。
        std::this_thread::sleep_for(std::chrono::milliseconds(210));
        EXPECT_TRUE(manager.restart_on_error().has_value());
        EXPECT_EQ(manager.state(), PlaybackState::Running);
        EXPECT_GT(mock_ptr->start_attempts(), attempts_after_first);

        manager.stop();
    }

    // ---- 事件归属（provenance）契约 ----
    // backend 事件必须带 stream generation，manager 只放行当前世代。没有这层
    // 契约，「我刚起的流死了」与「上一条流的迟到讣告」在上层不可判别。

    TEST(PlaybackManagerProvenanceTest, StaleGenerationEventIsDropped)
    {
        auto mock = std::make_unique<MockAudioPlayback>(
            MockAudioPlayback::Behavior { .threaded = false });
        auto* mock_ptr = mock.get();
        PlaybackManager manager(std::move(mock));

        std::vector<AudioError> events;
        ASSERT_TRUE(manager
                .start(make_playback_config(),
                    [](std::span<std::byte>) noexcept { return 0U; },
                    [&events](const AudioStreamEvent& event) noexcept {
                        events.push_back(event.error);
                    })
                .has_value());

        const auto live = mock_ptr->generation();
        ASSERT_NE(live, kNoStreamGeneration);
        ASSERT_EQ(manager.stale_events_dropped(), 0U);

        // 旧世代的迟到讣告：必须被丢弃，且不得触碰任何共享状态。
        mock_ptr->fire_event_with_generation(AudioError::DeviceDisconnected, live - 1);
        EXPECT_TRUE(events.empty());
        EXPECT_EQ(manager.stale_events_dropped(), 1U);
        EXPECT_EQ(manager.state(), PlaybackState::Running);
        EXPECT_TRUE(manager.is_running());

        // 当前世代照常放行。
        mock_ptr->fire_event_with_generation(AudioError::DeviceDisconnected, live);
        ASSERT_EQ(events.size(), 1U);
        EXPECT_EQ(events.front(), AudioError::DeviceDisconnected);
        EXPECT_EQ(manager.stale_events_dropped(), 1U);

        manager.stop();
    }

    // 事务期间 manager 先撤销认领（active_generation_ = kNoStreamGeneration）
    // 再 stop 旧流，因此 teardown 阶段 backend 投递的临终事件在 manager 层即
    // 被丢弃，不再依赖 ClientRuntime 用 PlaybackState::Switching 猜时间窗。
    TEST(PlaybackManagerProvenanceTest, EventDuringTeardownIsDroppedByUnclaimedGeneration)
    {
        auto mock = std::make_unique<MockAudioPlayback>(
            MockAudioPlayback::Behavior { .threaded = false });
        auto* mock_ptr = mock.get();
        PlaybackManager manager(std::move(mock));

        std::vector<AudioError> events;
        ASSERT_TRUE(manager
                .start(make_playback_config(),
                    [](std::span<std::byte>) noexcept { return 0U; },
                    [&events](const AudioStreamEvent& event) noexcept {
                        events.push_back(event.error);
                    })
                .has_value());
        const auto first_generation = mock_ptr->generation();

        // set_playback_device 的事务会 stop 旧流；模拟旧流在 teardown 窗口内
        // 投递临终错误（用 stop() 之后仍然可见的旧世代号）。
        ASSERT_TRUE(manager.set_playback_device(AudioDeviceId("usb-dac")).has_value());
        EXPECT_NE(mock_ptr->generation(), first_generation); // 确实是新的一条流
        mock_ptr->fire_event_with_generation(
            AudioError::DeviceDisconnected, first_generation);

        EXPECT_TRUE(events.empty());
        EXPECT_EQ(manager.stale_events_dropped(), 1U);
        EXPECT_EQ(manager.state(), PlaybackState::Running);
        // 新流没有被旧流的讣告带崩。
        EXPECT_TRUE(manager.is_running());

        manager.stop();
    }

    // ---- tick 驱动的设备集合轮询（Windows 侧唯一触发源）----

    // 回归：notify_devices_changed 只有 JNI 调用者，而 tick() 原先对
    // owner != None 直接返回 —— 于是 Windows client 钉住的设备拔掉后能回落
    // （流错误事件驱动），插回来却永远不切回。本测试锁定轮询路径把两段都接上。
    TEST(PlaybackManagerDevicePollTest, PinnedDeviceLossAndReturnAreDrivenByTick)
    {
        StubDeviceManager devices;
        devices.set_output({ "mock-default", "usb-dac" });
        devices.set_default_output("mock-default");

        auto mock = std::make_unique<MockAudioPlayback>(
            MockAudioPlayback::Behavior { .threaded = false });
        auto* mock_ptr = mock.get();
        PlaybackManager manager(std::move(mock), &devices);

        ASSERT_TRUE(manager
                .start(make_playback_config(AudioDeviceId("usb-dac")),
                    [](std::span<std::byte>) noexcept { return 0U; })
                .has_value());
        ASSERT_EQ(manager.intent_owner(), RouteIntentOwner::User);
        ASSERT_EQ(manager.stream_info().device_id.value(), "usb-dac");
        ASSERT_EQ(mock_ptr->start_attempts(), 1U);

        // 前两次 tick 建立基线（去抖 = 连续两次一致采样）：不得触发任何事务。
        EXPECT_FALSE(manager.tick());
        EXPECT_FALSE(manager.tick());
        ASSERT_EQ(mock_ptr->start_attempts(), 1U);

        // ---- 拔出 usb-dac ----
        mock_ptr->fail_device(AudioDeviceId("usb-dac"), AudioError::DeviceDisconnected);
        devices.set_output({ "mock-default" });
        // 单次采样不动作：WASAPI 在设备转换期可能瞬时枚举不全，一次就动手会
        // restart 一条健康的流。
        EXPECT_FALSE(manager.tick());
        EXPECT_EQ(mock_ptr->start_attempts(), 1U);
        // 第二次一致采样 -> eager restart -> 目标 usb-dac 失败 -> 回落系统默认。
        EXPECT_TRUE(manager.tick());
        EXPECT_EQ(manager.state(), PlaybackState::Running);
        EXPECT_EQ(manager.stream_info().device_id.value(), "mock-default");
        // 回落是临时降级：归属与 sticky 意图都不动（自动切回的前提）。
        EXPECT_EQ(manager.intent_owner(), RouteIntentOwner::User);
        ASSERT_TRUE(manager.preferred_or_active_device().has_value());
        EXPECT_EQ(manager.preferred_or_active_device()->value(), "usb-dac");

        // ---- 插回 usb-dac：自动切回 ----
        mock_ptr->clear_fail_rules();
        devices.set_output({ "mock-default", "usb-dac" });
        EXPECT_FALSE(manager.tick()); // 去抖第 1 次：仍停在兜底设备
        EXPECT_EQ(manager.stream_info().device_id.value(), "mock-default");
        EXPECT_TRUE(manager.tick());
        EXPECT_EQ(manager.state(), PlaybackState::Running);
        EXPECT_EQ(manager.stream_info().device_id.value(), "usb-dac");

        manager.stop();
    }

    // 跟随系统的会话：tick 第 2 步（默认设备比较）与第 1 步（设备集合去抖）
    // 对同一次插拔不得各切一次。默认值是权威读数，无需去抖，所以第 2 步先动手；
    // 随后集合变化被确认时，第 1 步必须发现"默认已跟上"而不再切。
    TEST(PlaybackManagerDevicePollTest, DefaultFollowAndSetChangeDoNotDoubleSwitch)
    {
        StubDeviceManager devices;
        devices.set_output({ "mock-default" });
        devices.set_default_output("mock-default");

        auto mock = std::make_unique<MockAudioPlayback>(
            MockAudioPlayback::Behavior { .threaded = false });
        auto* mock_ptr = mock.get();
        PlaybackManager manager(std::move(mock), &devices);

        ASSERT_TRUE(manager
                .start(make_playback_config(),
                    [](std::span<std::byte>) noexcept { return 0U; })
                .has_value());
        ASSERT_EQ(manager.intent_owner(), RouteIntentOwner::None);
        ASSERT_EQ(manager.stream_info().device_id.value(), "mock-default");

        EXPECT_FALSE(manager.tick()); // 去抖
        EXPECT_FALSE(manager.tick()); // 基线
        ASSERT_EQ(mock_ptr->start_attempts(), 1U);

        // usb-dac 插入并成为系统默认。mock 的 nullopt 回读必须与 stub 的默认值
        // 一致，否则第 2 步会每轮都判定"默认又变了"。
        devices.set_output({ "mock-default", "usb-dac" });
        devices.set_default_output("usb-dac");
        mock_ptr->set_system_default_device(AudioDeviceId("usb-dac"));

        // 第 2 步立即跟随（设备集合此刻还在去抖中）。
        EXPECT_TRUE(manager.tick());
        EXPECT_EQ(manager.state(), PlaybackState::Running);
        EXPECT_EQ(manager.stream_info().device_id.value(), "usb-dac");
        EXPECT_EQ(mock_ptr->start_attempts(), 2U);

        // 集合变化此时才被确认：第 1 步发现默认已跟上 -> 不切第二次。
        EXPECT_FALSE(manager.tick());
        EXPECT_EQ(mock_ptr->start_attempts(), 2U);

        // 之后一切安静：无变化不得产生周期性重开。
        for (int i = 0; i < 3; ++i) {
            EXPECT_FALSE(manager.tick()) << "iteration " << i;
        }
        EXPECT_EQ(mock_ptr->start_attempts(), 2U);
        EXPECT_EQ(manager.stream_info().device_id.value(), "usb-dac");

        manager.stop();
    }

    // 插入的设备与当前路由无关（系统默认没变）：跟随系统的会话不得重开流。
    // 这是 DeviceSetPoller 每 500ms 采样带来的新风险面，必须显式锁住。
    TEST(PlaybackManagerDevicePollTest, UnrelatedDeviceArrivalDoesNotRestartStream)
    {
        StubDeviceManager devices;
        devices.set_output({ "mock-default" });
        devices.set_default_output("mock-default");

        auto mock = std::make_unique<MockAudioPlayback>(
            MockAudioPlayback::Behavior { .threaded = false });
        auto* mock_ptr = mock.get();
        PlaybackManager manager(std::move(mock), &devices);

        ASSERT_TRUE(manager
                .start(make_playback_config(),
                    [](std::span<std::byte>) noexcept { return 0U; })
                .has_value());
        EXPECT_FALSE(manager.tick());
        EXPECT_FALSE(manager.tick());
        ASSERT_EQ(mock_ptr->start_attempts(), 1U);

        // 新设备到达但系统默认仍是 mock-default。
        devices.set_output({ "mock-default", "usb-dac" });
        for (int i = 0; i < 4; ++i) {
            EXPECT_FALSE(manager.tick()) << "iteration " << i;
        }
        EXPECT_EQ(mock_ptr->start_attempts(), 1U);
        EXPECT_EQ(manager.stream_info().device_id.value(), "mock-default");

        manager.stop();
    }

} // namespace
} // namespace aqua::audio
