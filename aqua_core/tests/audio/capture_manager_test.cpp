// CaptureManager restart 事务专项测试（capture_switching_design.md §11 Phase S1）。
//
// 覆盖底线验证（mock 后端 + mock 设备管理器，跨平台不依赖真实音频设备）：
//   1. 候选链：Switched / RolledBack / FellBackToSystem / Fatal（链耗尽）；
//   2. 共享重试预算：错误驱动 + 默认变化驱动合并计数（10s/3），超限 Fatal
//      且不触碰后端；
//   3. 格式不可变：首流后候选收到显式会话格式；backend 未履约（info 不符）
//      时候选按 FormatUnsupported 处理；
//   4. RestartWhileCallbackActive：回调线程推流时发起 restart，无死锁、
//      无双重生产（break-before-make 的 join 交接）；
//   5. tick 跟随：FollowSystem 轮询默认设备变化并跟随（返回值 = 是否
//      执行了事务）；PreferredDevice 不动作；跟随事务 stop 阶段投递的
//      旧流临终错误必须观察到 Switching（runtime 侧 Switching gate 的
//      前提，防二次 restart）。
//
// 时间线连续性（restart 前后 seq 单调、session 不重建）由实现结构保证：
// restart 只调用 CaptureManager 自身方法，packetizer/network/session 不在
// 其可达范围（代码评审项，对称 playback_manager_test 的对应说明）。

#include "aqua/audio/capture/capture_manager.h"
#include "aqua/audio/devices/audio_device_manager.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <expected>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <span>
#include <thread>
#include <utility>
#include <vector>

namespace aqua::audio {
namespace {

    AudioFormat make_format()
    {
        AudioFormat format;
        format.encoding = audio::AudioEncoding::PCM_S16LE;
        format.channels = 2;
        format.sample_rate = 48000;
        return format;
    }

    AudioCaptureConfig make_capture_config(std::optional<AudioDeviceId> device = std::nullopt)
    {
        AudioCaptureConfig config;
        config.source = AudioCaptureSource::OUTPUT_LOOPBACK;
        config.route = AudioRoute::from_optional(std::move(device));
        config.format = std::nullopt; // 由 backend 决定（首流后 manager 钉死）
        config.frames_per_buffer = 0;
        return config;
    }

    // 可编排的采集后端 mock（对称 playback_manager_test 的 MockAudioPlayback）：
    //   - threaded 模式：后台线程按 interval 周期推 block（模拟真实音频线程）；
    //   - manual 模式：测试手动 invoke_callback()（确定性时序）。
    // stop() 遵守 AudioCapture 契约：join 音频线程后才返回。
    class MockAudioCapture final : public AudioCapture {
    public:
        struct Behavior {
            bool threaded = false;
            std::chrono::milliseconds start_delay { 0 }; // start() 内耗时（模拟设备打开）
            std::chrono::milliseconds push_interval { 1 }; // 回调节奏
            // stop() 内经 event 回调回放一次旧流临终 DeviceDisconnected（模拟
            // WASAPI stop 的 join 竞态：stop SetEvent(error_event) 前，event
            // 线程可能已消费真实错误并回调——错误恰在切换事务 stop 阶段投递）。
            bool fire_event_on_stop = false;
        };

        explicit MockAudioCapture(Behavior behavior = { })
            : behavior_(behavior)
        {
        }

        ~MockAudioCapture() override { stop(); }

        // 可编排失败：对指定 device 的 start() 返回该错误（nullopt 匹配
        // "跟随系统"候选——注意 manager 会把候选解析成具体 id 后再调用
        // backend，所以失败规则应使用解析后的设备 id）。
        void fail_device(std::optional<AudioDeviceId> device, AudioError error)
        {
            fail_rules_.emplace_back(std::move(device), error);
        }

        void clear_fail_rules()
        {
            fail_rules_.clear();
        }

        // 格式违约编排：start 成功但 info() 报告与会话格式不符（模拟 backend
        // 未严格履约；manager 的 belt-and-braces 复核应判该候选
        // FormatUnsupported）。
        void report_wrong_format(bool enable)
        {
            wrong_format_ = enable;
        }

        std::expected<void, AudioError> start(const AudioCaptureConfig& config,
            AudioCaptureCallback block_callback,
            AudioCaptureEventCallback event_callback) noexcept override
        {
            start_attempts_.fetch_add(1, std::memory_order_relaxed);
            // 路由请求回读（旧 config.device 语义）：nullopt = "跟随系统"候选。
            const auto requested_device = config.route.endpoint_request();
            start_devices_.push_back(requested_device);
            start_formats_.push_back(config.format);
            if (running_.load(std::memory_order_acquire)) {
                return std::unexpected(AudioError::AlreadyRunning);
            }
            if (!block_callback) {
                return std::unexpected(AudioError::InvalidArgument);
            }
            for (const auto& [device, error] : fail_rules_) {
                if (device == requested_device) {
                    return std::unexpected(error);
                }
            }
            if (behavior_.start_delay > std::chrono::milliseconds::zero()) {
                std::this_thread::sleep_for(behavior_.start_delay);
            }
            block_callback_ = std::move(block_callback);
            event_callback_ = std::move(event_callback);
            info_.format = wrong_format_ ? make_wrong_format()
                                         : config.format.value_or(make_format());
            info_.frames_per_buffer = 480;
            stop_flag_.store(false, std::memory_order_release);
            start_calls_.fetch_add(1, std::memory_order_relaxed);
            // 认领新流：generation 每次成功 start 递增（从 1 起，0 保留给
            // 「无流」），manager 的 provenance 过滤据此判别事件归属。
            // 世代号取自单调计数器：stop() 会把 generation_ 归零，若共用一个
            // 原子则下一轮 start() 会重号，旧流的临终事件就能冒充新流
            // （与生产 backend 同一约束）。
            generation_.store(
                generation_counter_.fetch_add(1, std::memory_order_acq_rel) + 1,
                std::memory_order_release);
            running_.store(true, std::memory_order_release);
            if (behavior_.threaded) {
                thread_ = std::jthread(&MockAudioCapture::thread_main, this);
            }
            return { };
        }

        [[nodiscard]] AudioCaptureInfo info() const noexcept override
        {
            return info_;
        }

        bool is_running() const noexcept override
        {
            return running_.load(std::memory_order_acquire);
        }

        [[nodiscard]] StreamGeneration generation() const noexcept override
        {
            return generation_.load(std::memory_order_acquire);
        }

        void stop() noexcept override
        {
            // 退役流的代号：fire_event_on_stop 的临终事件属于**这条**流
            // （而非之后的新流），必须在归零前先捕获。
            const StreamGeneration retiring_generation
                = generation_.load(std::memory_order_acquire);
            stop_flag_.store(true, std::memory_order_release);
            if (thread_.joinable() && thread_.get_id() != std::this_thread::get_id()) {
                thread_.join();
            }
            const bool was_running = running_.exchange(false, std::memory_order_acq_rel);
            // 撤销认领：归零后 manager 的 provenance 过滤丢弃本世代的迟到事件。
            generation_.store(kNoStreamGeneration, std::memory_order_release);
            if (was_running) {
                stop_calls_.fetch_add(1, std::memory_order_relaxed);
                if (behavior_.fire_event_on_stop && event_callback_) {
                    event_callback_(AudioStreamEvent {
                        AudioError::DeviceDisconnected, retiring_generation });
                }
            }
            block_callback_ = nullptr;
            event_callback_ = nullptr;
        }

        // manual 模式：驱动一次 block 回调。
        void invoke_callback() noexcept
        {
            if (block_callback_) {
                static const std::byte kPayload[960 * 4] { };
                const AudioBlock block { std::span<const std::byte>(kPayload) };
                block_callback_(block);
            }
        }

        // 模拟 backend 运行期错误事件（device invalidated 等）。
        // 事件按契约打上当前流代号（audio_stream_event.h）。
        void fire_event(AudioError error) noexcept
        {
            if (event_callback_) {
                event_callback_(AudioStreamEvent {
                    error, generation_.load(std::memory_order_acquire) });
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
        // 每次 start() 的请求 device 序列（manager 解析后的具体 id）。
        [[nodiscard]] const std::vector<std::optional<AudioDeviceId>>& start_devices() const noexcept
        {
            return start_devices_;
        }
        // 每次 start() 的请求 format 序列（格式钉死断言）。
        [[nodiscard]] const std::vector<std::optional<AudioFormat>>& start_formats() const noexcept
        {
            return start_formats_;
        }
        // 并发回调观测：> 1 说明出现双重生产（两个回调线程同时存活）。
        [[nodiscard]] int max_concurrent_callbacks() const noexcept
        {
            return max_concurrent_.load(std::memory_order_relaxed);
        }

    private:
        static AudioFormat make_wrong_format()
        {
            AudioFormat format;
            format.encoding = audio::AudioEncoding::PCM_S16LE;
            format.channels = 1; // 与会话格式（2ch）不符
            format.sample_rate = 44100;
            return format;
        }

        void thread_main() noexcept
        {
            static const std::byte kPayload[960 * 4] { };
            while (!stop_flag_.load(std::memory_order_acquire)) {
                const int entered = ++concurrent_;
                int observed = max_concurrent_.load(std::memory_order_relaxed);
                while (entered > observed
                    && !max_concurrent_.compare_exchange_weak(
                        observed, entered, std::memory_order_relaxed)) {
                }
                if (block_callback_) {
                    const AudioBlock block { std::span<const std::byte>(kPayload) };
                    block_callback_(block);
                }
                --concurrent_;
                std::this_thread::sleep_for(behavior_.push_interval);
            }
        }

        Behavior behavior_;
        AudioCaptureInfo info_ { };
        AudioCaptureCallback block_callback_;
        AudioCaptureEventCallback event_callback_;
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
        std::atomic<std::uint64_t> stop_calls_ { 0 };
        bool wrong_format_ = false;
        // 仅控制线程写（manager 生命周期方法同线程串行），测试断言冷读。
        std::vector<std::pair<std::optional<AudioDeviceId>, AudioError>> fail_rules_;
        std::vector<std::optional<AudioDeviceId>> start_devices_;
        std::vector<std::optional<AudioFormat>> start_formats_;
    };

    // 可编排的设备管理器 mock：两个方向各自的设备集合 + 可变默认设备。
    class MockAudioDeviceManager final : public AudioDeviceManager {
    public:
        void set_devices(AudioDeviceDirection direction, std::vector<AudioDevice> devices)
        {
            if (direction == AudioDeviceDirection::INPUT) {
                input_devices_ = std::move(devices);
            } else {
                output_devices_ = std::move(devices);
            }
        }

        void set_default(AudioDeviceDirection direction, AudioDeviceId id)
        {
            if (direction == AudioDeviceDirection::INPUT) {
                default_input_ = std::move(id);
            } else {
                default_output_ = std::move(id);
            }
        }

        [[nodiscard]] std::vector<AudioDevice>
        enumerate(AudioDeviceDirection direction) const override
        {
            return direction == AudioDeviceDirection::INPUT ? input_devices_ : output_devices_;
        }

        [[nodiscard]] std::optional<AudioDevice>
        default_device(AudioDeviceDirection direction) const override
        {
            const auto& id = direction == AudioDeviceDirection::INPUT ? default_input_ : default_output_;
            for (const auto& device : enumerate(direction)) {
                if (device.id == id) {
                    return device;
                }
            }
            return std::nullopt;
        }

        [[nodiscard]] std::expected<AudioFormat, AudioError>
        default_format(AudioDeviceDirection direction,
            const std::optional<AudioDeviceId>& requested) const override
        {
            const auto resolved = resolve(direction, requested);
            if (!resolved) {
                return std::unexpected(resolved.error());
            }
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
        std::vector<AudioDevice> input_devices_;
        std::vector<AudioDevice> output_devices_;
        AudioDeviceId default_input_;
        AudioDeviceId default_output_;
    };

    AudioDevice make_device(const char* id, AudioDeviceDirection direction)
    {
        AudioDevice device;
        device.id = AudioDeviceId(id);
        device.name = id;
        device.direction = direction;
        device.is_default = false;
        return device;
    }

    // 标准 loopback 环境：输出方向 d1/d2，默认 d1。
    std::unique_ptr<MockAudioDeviceManager> make_loopback_devices()
    {
        auto manager = std::make_unique<MockAudioDeviceManager>();
        manager->set_devices(AudioDeviceDirection::OUTPUT,
            { make_device("d1", AudioDeviceDirection::OUTPUT),
                make_device("d2", AudioDeviceDirection::OUTPUT) });
        manager->set_default(AudioDeviceDirection::OUTPUT, AudioDeviceId("d1"));
        return manager;
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

    TEST(CaptureManagerTest, StartStopStateTransitions)
    {
        auto mock = std::make_unique<MockAudioCapture>();
        auto* mock_ptr = mock.get();
        CaptureManager manager(std::move(mock));

        EXPECT_EQ(manager.state(), CaptureSwitchState::Inactive);
        EXPECT_FALSE(manager.is_running());
        EXPECT_TRUE(manager.available());

        // 尚未 start 过：restart 无"旧配置"，拒绝。
        const auto early = manager.restart_on_error();
        ASSERT_FALSE(early.has_value());
        EXPECT_EQ(early.error(), AudioError::NotRunning);

        const auto started = manager.start(make_capture_config(),
            [](const AudioBlock&) noexcept { });
        ASSERT_TRUE(started.has_value());
        EXPECT_EQ(manager.state(), CaptureSwitchState::Running);
        EXPECT_TRUE(manager.is_running());
        // 无显式设备 -> FollowSystem。
        EXPECT_EQ(manager.intent_owner(), RouteIntentOwner::None);
        EXPECT_FALSE(manager.preferred_device().has_value());

        manager.stop();
        EXPECT_EQ(manager.state(), CaptureSwitchState::Inactive);
        EXPECT_FALSE(manager.is_running());
        EXPECT_EQ(mock_ptr->stop_calls(), 1U);
    }

    TEST(CaptureManagerTest, StartWithEmptyCallbackRejected)
    {
        CaptureManager manager(std::make_unique<MockAudioCapture>());
        const auto result = manager.start(make_capture_config(), nullptr);
        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error(), AudioError::InvalidArgument);
        EXPECT_EQ(manager.state(), CaptureSwitchState::Inactive);
    }

    TEST(CaptureManagerTest, ExplicitDeviceStartsPreferredRoute)
    {
        auto devices = make_loopback_devices();
        CaptureManager manager(std::make_unique<MockAudioCapture>(), devices.get());

        const auto started = manager.start(make_capture_config(AudioDeviceId("d2")),
            [](const AudioBlock&) noexcept { });
        ASSERT_TRUE(started.has_value());
        EXPECT_EQ(manager.intent_owner(), RouteIntentOwner::User);
        ASSERT_TRUE(manager.preferred_device().has_value());
        EXPECT_EQ(*manager.preferred_device(), AudioDeviceId("d2"));
        ASSERT_TRUE(manager.active_device().has_value());
        EXPECT_EQ(*manager.active_device(), AudioDeviceId("d2"));
        manager.stop();
    }

    TEST(CaptureManagerTest, StartResolveFailureLeavesInactive)
    {
        auto devices = make_loopback_devices();
        CaptureManager manager(std::make_unique<MockAudioCapture>(), devices.get());

        // 指定设备不存在：候选解析失败，start 即失败。
        const auto started = manager.start(make_capture_config(AudioDeviceId("gone")),
            [](const AudioBlock&) noexcept { });
        ASSERT_FALSE(started.has_value());
        EXPECT_EQ(started.error(), AudioError::DeviceNotFound);
        EXPECT_EQ(manager.state(), CaptureSwitchState::Inactive);
        manager.stop();
    }

    // ---- 候选链 ----

    TEST(CaptureManagerSwitchTest, ErrorRestartFollowSystemTargetsSystemDefault)
    {
        auto devices = make_loopback_devices();
        auto mock = std::make_unique<MockAudioCapture>();
        auto* mock_ptr = mock.get();
        CaptureManager manager(std::move(mock), devices.get());

        ASSERT_TRUE(manager.start(make_capture_config(),
                               [](const AudioBlock&) noexcept { })
                .has_value());
        ASSERT_EQ(manager.intent_owner(), RouteIntentOwner::None);
        ASSERT_EQ(*manager.active_device(), AudioDeviceId("d1"));

        const auto result = manager.restart_on_error();
        ASSERT_TRUE(result.has_value()) << audio::audio_error_name(result.error());
        EXPECT_EQ(result->outcome, SwitchOutcome::Switched);
        EXPECT_EQ(manager.state(), CaptureSwitchState::Running);
        // 候选 [nullopt, previous(d1)]：首尝试 nullopt 即按当时默认 d1 成功，
        // 不再试 previous（链短路返回，不是去重删掉 previous）。
        EXPECT_EQ(mock_ptr->start_calls(), 2U);
        EXPECT_EQ(*manager.active_device(), AudioDeviceId("d1"));
        manager.stop();
    }

    TEST(CaptureManagerSwitchTest, ErrorRestartPreferredPinnedDeviceGoneGoesFatal)
    {
        auto devices = make_loopback_devices();
        auto mock = std::make_unique<MockAudioCapture>();
        auto* mock_ptr = mock.get();
        CaptureManager manager(std::move(mock), devices.get());

        ASSERT_TRUE(manager.start(make_capture_config(AudioDeviceId("d2")),
                               [](const AudioBlock&) noexcept { })
                .has_value());

        // 钉住的 d2 故障：候选链 [d2] 耗尽 -> Fatal，绝不降级到系统默认 d1
        // （显式 --capture-device-id = "只要这个设备的数据"）。
        mock_ptr->fail_device(AudioDeviceId("d2"), AudioError::DeviceDisconnected);
        const auto result = manager.restart_on_error();
        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error(), AudioError::DeviceDisconnected);
        EXPECT_EQ(manager.state(), CaptureSwitchState::Fatal);
        // 只尝试过 d2 一个候选，没有兜底 d1 的 start。
        ASSERT_EQ(mock_ptr->start_devices().size(), 2U); // 初始 d2 + restart d2
        EXPECT_EQ(mock_ptr->start_devices().back(),
            std::optional<AudioDeviceId>(AudioDeviceId("d2")));
        // sticky 意图保持（route_mode / preferred_device 仍是 d2）。
        EXPECT_EQ(manager.intent_owner(), RouteIntentOwner::User);
        EXPECT_EQ(*manager.preferred_device(), AudioDeviceId("d2"));
        manager.stop();
    }

    TEST(CaptureManagerSwitchTest, ChainExhaustedGoesFatalAndStaysTerminal)
    {
        auto devices = make_loopback_devices();
        auto mock = std::make_unique<MockAudioCapture>();
        auto* mock_ptr = mock.get();
        CaptureManager manager(std::move(mock), devices.get());

        ASSERT_TRUE(manager.start(make_capture_config(AudioDeviceId("d2")),
                               [](const AudioBlock&) noexcept { })
                .has_value());

        // PreferredDevice 的候选链只有 [d2]：d2 故障即链耗尽 -> Fatal。
        mock_ptr->fail_device(AudioDeviceId("d2"), AudioError::DeviceDisconnected);
        const auto result = manager.restart_on_error();
        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(manager.state(), CaptureSwitchState::Fatal);
        const auto switch_result = manager.last_switch_result();
        ASSERT_TRUE(switch_result.has_value());
        EXPECT_EQ(switch_result->outcome, SwitchOutcome::Fatal);

        // Fatal 是终态：后续 restart 拒绝且不触碰后端。
        mock_ptr->clear_fail_rules();
        const auto attempts = mock_ptr->start_attempts();
        const auto again = manager.restart_on_error();
        ASSERT_FALSE(again.has_value());
        EXPECT_EQ(mock_ptr->start_attempts(), attempts);
        EXPECT_EQ(manager.state(), CaptureSwitchState::Fatal);
        manager.stop();
    }

    // ---- 共享重试预算（10s/3：错误驱动 + 默认变化驱动合并计数）----

    TEST(CaptureManagerSwitchTest, RetryBudgetExhaustedGoesFatalWithoutTouchingBackend)
    {
        auto devices = make_loopback_devices();
        auto mock = std::make_unique<MockAudioCapture>();
        auto* mock_ptr = mock.get();
        CaptureManager manager(std::move(mock), devices.get());

        ASSERT_TRUE(manager.start(make_capture_config(),
                               [](const AudioBlock&) noexcept { })
                .has_value());

        // 预算内 3 次自动 restart 成功。
        for (int i = 0; i < 3; ++i) {
            const auto result = manager.restart_on_error();
            ASSERT_TRUE(result.has_value()) << "restart " << i << " should be within budget";
            EXPECT_EQ(manager.state(), CaptureSwitchState::Running);
        }
        EXPECT_EQ(mock_ptr->start_calls(), 4U); // 初始 + 3 次 restart

        // 第 4 次（窗口内）：超限按链耗尽处理 -> Fatal，且不触碰后端。
        const auto attempts = mock_ptr->start_attempts();
        const auto fourth = manager.restart_on_error();
        ASSERT_FALSE(fourth.has_value());
        EXPECT_EQ(manager.state(), CaptureSwitchState::Fatal);
        EXPECT_EQ(mock_ptr->start_attempts(), attempts);
        manager.stop();
    }

    // ---- 格式不可变 ----

    TEST(CaptureManagerSwitchTest, SessionFormatPinnedAcrossRestart)
    {
        auto devices = make_loopback_devices();
        auto mock = std::make_unique<MockAudioCapture>();
        auto* mock_ptr = mock.get();
        CaptureManager manager(std::move(mock), devices.get());

        // 首流 format=nullopt：backend 选定 48k/2ch/S16（mock 默认）。
        ASSERT_TRUE(manager.start(make_capture_config(),
                               [](const AudioBlock&) noexcept { })
                .has_value());
        ASSERT_FALSE(mock_ptr->start_formats().front().has_value());

        // restart：候选必须收到显式会话格式（Format immutable 的载体）。
        const auto result = manager.restart_on_error();
        ASSERT_TRUE(result.has_value());
        ASSERT_EQ(mock_ptr->start_formats().size(), 2U);
        ASSERT_TRUE(mock_ptr->start_formats().back().has_value());
        EXPECT_EQ(*mock_ptr->start_formats().back(), make_format());
        manager.stop();
    }

    TEST(CaptureManagerSwitchTest, BackendFormatViolationFailsCandidate)
    {
        auto devices = make_loopback_devices();
        auto mock = std::make_unique<MockAudioCapture>();
        auto* mock_ptr = mock.get();
        CaptureManager manager(std::move(mock), devices.get());

        ASSERT_TRUE(manager.start(make_capture_config(),
                               [](const AudioBlock&) noexcept { })
                .has_value());

        // backend 违约：start 成功但 info 与会话格式不符 -> 每个候选都按
        // FormatUnsupported 处理 -> 链耗尽 Fatal。
        mock_ptr->report_wrong_format(true);
        const auto result = manager.restart_on_error();
        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error(), AudioError::FormatUnsupported);
        EXPECT_EQ(manager.state(), CaptureSwitchState::Fatal);
        manager.stop();
    }

    // ---- Test 4：restart while callback active（join 死锁底线）----

    TEST(CaptureManagerRestartTest, RestartWhileCallbackActiveIsSafe)
    {
        auto devices = make_loopback_devices();
        auto mock = std::make_unique<MockAudioCapture>(
            MockAudioCapture::Behavior { .threaded = true });
        auto* mock_ptr = mock.get();
        CaptureManager manager(std::move(mock), devices.get());

        std::atomic<std::uint64_t> blocks { 0 };
        const auto started = manager.start(make_capture_config(),
            [&blocks](const AudioBlock&) noexcept {
                blocks.fetch_add(1, std::memory_order_relaxed);
            });
        ASSERT_TRUE(started.has_value());

        // 等回调线程真正跑起来（回调正在推流的竞争窗口内发起 restart）。
        ASSERT_TRUE(wait_for([&] { return blocks.load() >= 20; }))
            << "capture callback did not start producing";

        const auto blocks_before = blocks.load();

        // 控制线程发起 restart；死锁则超时失败（测试的底线目标）。
        auto restart_future = std::async(std::launch::async, [&] { return manager.restart_on_error(); });
        ASSERT_NE(restart_future.wait_for(std::chrono::seconds(5)), std::future_status::timeout)
            << "restart deadlocked while callback active";
        const auto restarted = restart_future.get();
        ASSERT_TRUE(restarted.has_value()) << audio::audio_error_name(restarted.error());
        EXPECT_EQ(manager.state(), CaptureSwitchState::Running);
        EXPECT_TRUE(manager.is_running());
        EXPECT_EQ(mock_ptr->start_calls(), 2U);

        // 无双重生产：任何时刻至多一个回调线程在推流；restart 后生产继续。
        ASSERT_TRUE(wait_for([&] { return blocks.load() > blocks_before + 10; }))
            << "no blocks produced after restart";
        EXPECT_EQ(mock_ptr->max_concurrent_callbacks(), 1);
        manager.stop();
    }

    // ---- tick 跟随系统默认（§6 路径 2）----

    TEST(CaptureManagerTickTest, FollowSystemFollowsDefaultChange)
    {
        auto devices = make_loopback_devices();
        auto* devices_ptr = devices.get();
        auto mock = std::make_unique<MockAudioCapture>();
        auto* mock_ptr = mock.get();
        CaptureManager manager(std::move(mock), devices.get());

        ASSERT_TRUE(manager.start(make_capture_config(),
                               [](const AudioBlock&) noexcept { })
                .has_value());
        ASSERT_EQ(*manager.active_device(), AudioDeviceId("d1"));

        // 默认未变化：tick 不动作（无事务）。
        EXPECT_FALSE(manager.tick());
        EXPECT_EQ(mock_ptr->start_calls(), 1U);

        // 默认变化 d1 -> d2：tick 跟随（restart 到新默认），返回已执行事务。
        devices_ptr->set_default(AudioDeviceDirection::OUTPUT, AudioDeviceId("d2"));
        EXPECT_TRUE(manager.tick());
        EXPECT_EQ(manager.state(), CaptureSwitchState::Running);
        EXPECT_EQ(*manager.active_device(), AudioDeviceId("d2"));
        EXPECT_EQ(mock_ptr->start_calls(), 2U);
        const auto switch_result = manager.last_switch_result();
        ASSERT_TRUE(switch_result.has_value());
        EXPECT_EQ(switch_result->outcome, SwitchOutcome::Switched);
        manager.stop();
    }

    TEST(CaptureManagerTickTest, PreferredDeviceDoesNotFollowDefaultChange)
    {
        auto devices = make_loopback_devices();
        auto* devices_ptr = devices.get();
        auto mock = std::make_unique<MockAudioCapture>();
        auto* mock_ptr = mock.get();
        CaptureManager manager(std::move(mock), devices.get());

        ASSERT_TRUE(manager.start(make_capture_config(AudioDeviceId("d2")),
                               [](const AudioBlock&) noexcept { })
                .has_value());

        // PreferredDevice：默认变化不驱动任何动作（用户意图优先，无事务）。
        devices_ptr->set_default(AudioDeviceDirection::OUTPUT, AudioDeviceId("d1"));
        EXPECT_FALSE(manager.tick());
        EXPECT_EQ(mock_ptr->start_calls(), 1U);
        EXPECT_EQ(*manager.active_device(), AudioDeviceId("d2"));
        manager.stop();
    }

    TEST(CaptureManagerTickTest, DefaultFollowSharesRetryBudget)
    {
        auto devices = make_loopback_devices();
        auto* devices_ptr = devices.get();
        auto mock = std::make_unique<MockAudioCapture>();
        CaptureManager manager(std::move(mock), devices.get());

        ASSERT_TRUE(manager.start(make_capture_config(),
                               [](const AudioBlock&) noexcept { })
                .has_value());

        // 错误驱动消耗全部 3 次预算。
        for (int i = 0; i < 3; ++i) {
            ASSERT_TRUE(manager.restart_on_error().has_value());
        }

        // 默认变化驱动的 follow 与错误驱动共享预算：窗口内第 4 次 -> Fatal
        // （预算拒绝，未执行事务，tick 返回 false）。
        devices_ptr->set_default(AudioDeviceDirection::OUTPUT, AudioDeviceId("d2"));
        EXPECT_FALSE(manager.tick());
        EXPECT_EQ(manager.state(), CaptureSwitchState::Fatal);
        manager.stop();
    }

    // 回归（二次 restart 状态机缺陷）：tick 驱动的跟随事务期间，旧流临终
    // DeviceDisconnected（stop 的 join 竞态窗口内投递）绝不能触达 runtime——
    // 否则 ServerRuntime 会置 pending 标志，下一 control tick 对同一次设备
    // 变化做第二次 restart。
    //
    // 拦截点已从 runtime 上移到 manager：switch_to 在 capture_->stop() 之前
    // 先撤销对流世代的认领（active_generation_ = kNoStreamGeneration），临终
    // 事件带着旧世代到达 start_stream 的 provenance 过滤即被丢弃，根本不进
    // app 回调。这比原先靠 ServerRuntime::on_capture_event 观察
    // state()==Switching 的时间窗判据更精确：时间窗在"新流已起、状态尚未翻
    // 回 Running"的缝隙里会漏判，而世代号不会。
    // runtime 侧的 Switching gate 保留为第二道防线。
    TEST(CaptureManagerTickTest, StaleErrorDuringFollowTransactionDroppedByGeneration)
    {
        auto devices = make_loopback_devices();
        auto* devices_ptr = devices.get();
        MockAudioCapture::Behavior behavior;
        behavior.fire_event_on_stop = true; // 旧流临终错误恰在事务 stop 阶段投递
        auto mock = std::make_unique<MockAudioCapture>(behavior);
        auto* mock_ptr = mock.get();
        CaptureManager manager(std::move(mock), devices.get());

        std::vector<CaptureSwitchState> observed_on_error;
        ASSERT_TRUE(manager.start(make_capture_config(), [](const AudioBlock&) noexcept { }, [&manager, &observed_on_error](const AudioStreamEvent& event) noexcept {
            if (event.error == AudioError::DeviceDisconnected) {
                // 测试专用观测（破坏"回调禁调本类方法"约定中的查询类
                // 例外）：state() 是无锁原子读，不与切换事务产生锁交互；
                // 验证的正是回调视角的管理状态。
                observed_on_error.push_back(manager.state());
            } }).has_value());
        ASSERT_EQ(*manager.active_device(), AudioDeviceId("d1"));
        ASSERT_EQ(manager.stale_events_dropped(), 0U);

        // 默认变化 d1 -> d2：tick 跟随，事务 stop 阶段回放旧流错误。
        devices_ptr->set_default(AudioDeviceDirection::OUTPUT, AudioDeviceId("d2"));
        EXPECT_TRUE(manager.tick());
        EXPECT_EQ(manager.state(), CaptureSwitchState::Running);
        EXPECT_EQ(*manager.active_device(), AudioDeviceId("d2"));
        // 只有跟随事务自己那一次重开：临终错误没有诱发第二次 restart。
        EXPECT_EQ(mock_ptr->start_calls(), 2U);

        // 临终事件被 provenance 过滤拦下，既未触达 app 回调，也被如实计数
        // （计数是判断"过滤真的在起作用"的唯一可观测量，不能只看回调没来）。
        EXPECT_TRUE(observed_on_error.empty());
        EXPECT_EQ(manager.stale_events_dropped(), 1U);
        manager.stop();
    }

    // ---- 设备集合轮询（tick 第 1 步）----

    // 用户要求："Server 如果指定设备断了就直接退出"。此前这只在 backend 投递
    // 流错误时成立；轮询把它变成主动保证——端点挂死而不报错时也能退出。
    TEST(CaptureManagerTickTest, PinnedDeviceVanishingFromPollGoesFatal)
    {
        auto devices = make_loopback_devices();
        auto* devices_ptr = devices.get();
        auto mock = std::make_unique<MockAudioCapture>();
        CaptureManager manager(std::move(mock), devices.get());

        ASSERT_TRUE(manager.start(make_capture_config(AudioDeviceId("d2")),
                               [](const AudioBlock&) noexcept { })
                .has_value());
        ASSERT_EQ(*manager.active_device(), AudioDeviceId("d2"));

        // 基线：去抖需连续两次一致采样，首次采样不上报。
        EXPECT_FALSE(manager.tick());
        EXPECT_FALSE(manager.tick());
        ASSERT_EQ(manager.state(), CaptureSwitchState::Running);

        // d2 被拔出：单次采样不得动作（枚举可能瞬时不全）。
        devices_ptr->set_devices(AudioDeviceDirection::OUTPUT,
            { make_device("d1", AudioDeviceDirection::OUTPUT) });
        EXPECT_FALSE(manager.tick());
        EXPECT_EQ(manager.state(), CaptureSwitchState::Running);

        // 第二次一致采样 -> eager restart_on_error -> 钉住归属候选链只有 [d2]
        // -> 解析失败 -> 链耗尽 -> Fatal（决策者据此停 server）。
        EXPECT_TRUE(manager.tick());
        EXPECT_EQ(manager.state(), CaptureSwitchState::Fatal);
        const auto switch_result = manager.last_switch_result();
        ASSERT_TRUE(switch_result.has_value());
        EXPECT_EQ(switch_result->outcome, SwitchOutcome::Fatal);
        manager.stop();
    }

    // 轮询每 500ms 采样一次，因此"插拔的是别的设备"必须完全不动作：
    // 钉住会话正在采集的端点没变，重开只会白白制造一次采集空档。
    TEST(CaptureManagerTickTest, PinnedSessionIgnoresUnrelatedDeviceChange)
    {
        auto devices = make_loopback_devices();
        auto* devices_ptr = devices.get();
        auto mock = std::make_unique<MockAudioCapture>();
        auto* mock_ptr = mock.get();
        CaptureManager manager(std::move(mock), devices.get());

        ASSERT_TRUE(manager.start(make_capture_config(AudioDeviceId("d2")),
                               [](const AudioBlock&) noexcept { })
                .has_value());
        EXPECT_FALSE(manager.tick());
        EXPECT_FALSE(manager.tick());
        ASSERT_EQ(mock_ptr->start_calls(), 1U);

        // d1 被拔出/插回，与正在采集的 d2 无关。
        devices_ptr->set_devices(AudioDeviceDirection::OUTPUT,
            { make_device("d2", AudioDeviceDirection::OUTPUT) });
        EXPECT_FALSE(manager.tick()); // 去抖
        EXPECT_FALSE(manager.tick()); // 确认：d2 仍在 -> 不动作
        EXPECT_EQ(manager.state(), CaptureSwitchState::Running);
        EXPECT_EQ(*manager.active_device(), AudioDeviceId("d2"));

        devices_ptr->set_devices(AudioDeviceDirection::OUTPUT,
            { make_device("d1", AudioDeviceDirection::OUTPUT),
                make_device("d2", AudioDeviceDirection::OUTPUT) });
        EXPECT_FALSE(manager.tick());
        EXPECT_FALSE(manager.tick());
        EXPECT_EQ(manager.state(), CaptureSwitchState::Running);
        EXPECT_EQ(mock_ptr->start_calls(), 1U);
        manager.stop();
    }

    // ---- 事件回调透传（backend event -> runtime 路径的载体）----

    TEST(CaptureManagerTest, EventCallbackPassthroughAcrossRestart)
    {
        auto devices = make_loopback_devices();
        auto mock = std::make_unique<MockAudioCapture>();
        auto* mock_ptr = mock.get();
        CaptureManager manager(std::move(mock), devices.get());

        std::vector<AudioError> events;
        ASSERT_TRUE(manager.start(make_capture_config(), [](const AudioBlock&) noexcept { }, [&events](const AudioStreamEvent& event) noexcept { events.push_back(event.error); }).has_value());

        mock_ptr->fire_event(AudioError::DeviceDisconnected);
        ASSERT_EQ(events.size(), 1U);
        EXPECT_EQ(events.front(), AudioError::DeviceDisconnected);

        // restart 后同一事件回调仍然生效（bundle 复用）。
        ASSERT_TRUE(manager.restart_on_error().has_value());
        mock_ptr->fire_event(AudioError::BackendFailed);
        ASSERT_EQ(events.size(), 2U);
        EXPECT_EQ(events.back(), AudioError::BackendFailed);
        manager.stop();
    }

} // namespace
} // namespace aqua::audio
