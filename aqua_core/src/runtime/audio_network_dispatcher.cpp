#include "aqua/runtime/audio_network_dispatcher.h"

#include "aqua/logger/logger.h"
#include "aqua/net/udp/network_frame.h"
#include "aqua/runtime/runtime_config.h"

#include <chrono>

#include <memory>
#include <system_error>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
// clang-format off: windows.h 必须先于 mmsystem.h（mmsystem.h 依赖 windows.h
// 基础类型），且 NOMINMAX 必须在 windows.h 之前定义；顺序 load-bearing，禁止排序。
#include <windows.h>
#include <mmsystem.h>
// clang-format on
#ifdef _MSC_VER
#pragma comment(lib, "winmm.lib")
#endif
#endif

namespace aqua::runtime {

namespace {

#ifdef _WIN32
    // Windows 默认定时器粒度 15.6ms：sleep_until(几 ms) 会被向上量化，pacing 间隔
    // （典型 3.6ms）完全失效——worker 一觉睡 15.6ms，队列必然涨到追赶深度，catchup
    // 一次性突发，接收端看到 20~33ms 周期性空隙（实测 Wi-Fi 链路 stall 日志证实）。
    // 优先用 CREATE_WAITABLE_TIMER_HIGH_RESOLUTION（Win10 1809+，亚毫秒精度、无
    // 系统级副作用）；创建失败的老系统退化为 timeBeginPeriod(1) + sleep_until。
    class PaceWaiter final {
    public:
        PaceWaiter() noexcept
        {
            timer_ = ::CreateWaitableTimerExW(nullptr, nullptr,
                CREATE_WAITABLE_TIMER_MANUAL_RESET | CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                TIMER_ALL_ACCESS);
            if (!timer_) {
                period_raised_ = (::timeBeginPeriod(1) == TIMERR_NOERROR);
            }
        }
        PaceWaiter(const PaceWaiter&) = delete;
        PaceWaiter& operator=(const PaceWaiter&) = delete;
        ~PaceWaiter()
        {
            if (timer_) {
                ::CloseHandle(timer_);
            }
            if (period_raised_) {
                ::timeEndPeriod(1);
            }
        }

        [[nodiscard]] const char* mode() const noexcept
        {
            return timer_ ? "high_res_timer"
                          : (period_raised_ ? "timeBeginPeriod(1ms)" : "sleep_until(default)");
        }

        void wait_until(std::chrono::steady_clock::time_point deadline) noexcept
        {
            if (!timer_) {
                std::this_thread::sleep_until(deadline);
                return;
            }
            // 可等待定时器的时间基准与 steady_clock 不同 → 用相对超时（负的 100ns
            // 计数）。允许晚醒（下一拍重算），不允许早于 deadline 空转，故循环兜底。
            for (;;) {
                const auto remaining = std::chrono::duration_cast<std::chrono::nanoseconds>(
                    deadline - std::chrono::steady_clock::now())
                                           .count();
                if (remaining <= 0) {
                    return;
                }
                LARGE_INTEGER due;
                due.QuadPart = -(remaining / 100);
                if (!::SetWaitableTimer(timer_, &due, 0, nullptr, nullptr, FALSE)) {
                    std::this_thread::sleep_until(deadline);
                    return;
                }
                ::WaitForSingleObject(timer_, INFINITE);
            }
        }

    private:
        HANDLE timer_ = nullptr;
        bool period_raised_ = false;
    };
#else
    // Linux/Android 的睡眠精度（hrtimer）足够覆盖 ms 级 pacing，无需特殊处理。
    class PaceWaiter final {
    public:
        [[nodiscard]] const char* mode() const noexcept
        {
            return "sleep_until";
        }
        void wait_until(std::chrono::steady_clock::time_point deadline) noexcept
        {
            std::this_thread::sleep_until(deadline);
        }
    };
#endif

} // namespace

AudioNetworkDispatcher::AudioNetworkDispatcher(
    audio::AudioFrameQueue& queue, net::UdpServer& udp) noexcept
    : queue_(queue)
    , udp_(udp)
{
    log_debug_fmt("AudioNetworkDispatcher configured: queue_capacity={} packet_frames={} frame_bytes={} slot_bytes={}",
        queue_.capacity_slots(), queue_.frame_count(), queue_.frame_bytes(), queue_.slot_bytes());
}

AudioNetworkDispatcher::~AudioNetworkDispatcher()
{
    stop();
}

bool AudioNetworkDispatcher::start()
{
    if (worker_.joinable()) {
        log_warn("AudioNetworkDispatcher::start called while already running");
        return false;
    }
    try {
        worker_ = std::jthread([this](std::stop_token st) {
            // stop 请求 → 唤醒 wait：worker 大部分时间阻塞在 wake_generation_.wait()，
            // 不唤醒就会一直睡到下一帧到达才退出。
            std::stop_callback cb(st, [this] {
                wake_generation_.fetch_add(1, std::memory_order_release);
                wake_generation_.notify_all();
            });
            run(st);
        });
        log_debug("AudioNetworkDispatcher started");
        return true;
    } catch (const std::system_error& e) {
        log_error_fmt("AudioNetworkDispatcher: failed to start worker thread: {}", format_exception_message(e));
        return false;
    } catch (...) {
        log_error("AudioNetworkDispatcher: failed to start worker thread");
        return false;
    }
}

void AudioNetworkDispatcher::stop() noexcept
{
    // request_stop() 同步执行线程体内注册的 stop_callback（唤醒 wake_generation_ 等待）。
    worker_.request_stop();
    if (worker_.joinable()) {
        worker_.join();
    }
    log_debug_fmt("AudioNetworkDispatcher stopped (paced_sends={} catchup_drains={} wakeups={})",
        paced_sends_.load(std::memory_order_relaxed),
        catchup_drains_.load(std::memory_order_relaxed),
        worker_wakeups_.load(std::memory_order_relaxed));
}

void AudioNetworkDispatcher::publish_from_realtime(bool should_notify) noexcept
{
    published_frames_.fetch_add(1, std::memory_order_relaxed);
    wake_generation_.fetch_add(1, std::memory_order_release);
    if (should_notify) {
        wake_generation_.notify_one();
    }
}

void AudioNetworkDispatcher::run(std::stop_token st) noexcept
{
    log_debug("AudioNetworkDispatcher worker entered");
    const bool paced = pacing_interval_ > std::chrono::nanoseconds::zero();
    PaceWaiter pace_waiter;
    if (paced) {
        log_debug_fmt("AudioNetworkDispatcher pacing enabled: interval={}ns wait_mode={}",
            pacing_interval_.count(), pace_waiter.mode());
    }
    auto next_send = std::chrono::steady_clock::now();
    while (!st.stop_requested()) {
        if (paced) {
            drain_paced(next_send);
        } else {
            drain();
        }
        if (!queue_.empty()) {
            if (paced) {
                // 队列未空但未到发送时刻：睡到下一时刻（间隔有界，stop 至多
                // 晚一个 packet 周期被观察到；期间到达的新帧留在队列里摊平）。
                // Windows 上必须走高精度等待，否则 15.6ms 粒度会量化掉 pacing。
                const auto now = std::chrono::steady_clock::now();
                if (now < next_send) {
                    pace_waiter.wait_until(next_send);
                }
            }
            continue;
        }

        const auto observed = wake_generation_.load(std::memory_order_acquire);
        if (queue_.empty() && !st.stop_requested()) {
            wake_generation_.wait(observed, std::memory_order_acquire);
            worker_wakeups_.fetch_add(1, std::memory_order_relaxed);
            if (paced) {
                // 队列睡空后重新锚定时刻表：cv 睡眠期间没有产出可发的帧，
                // 这段"欠账"不是真 backlog；若不锚定，唤醒时补发逻辑会把
                // 每个 capture block 的头两包背靠背打出（成对突发），接收端
                // 抖动估计被系统性抬高（实测 J≈3.2ms、target 高出地板 1~2 槽）。
                next_send = std::chrono::steady_clock::now();
            }
        }
    }
    drain();
    log_debug("AudioNetworkDispatcher worker exited");
}

// pacing 出队：队列深度达到追赶阈值说明 worker 被调度饿死过（或长时间无
// client 后堆积），一次性清空后重新起表；稳态 burst 深度低于阈值，不会误入。
// 正常路径走绝对时刻表（next_send += interval）：<一个间隔的唤醒延迟不累积；
// 欠账超过一个间隔时当拍补发（上限 DISPATCH_PACING_MAX_CATCHUP_SENDS 包）追平。
void AudioNetworkDispatcher::drain_paced(
    std::chrono::steady_clock::time_point& next_send) noexcept
{
    if (queue_.size_slots() >= config::DISPATCH_PACING_CATCHUP_DEPTH_SLOTS) {
        // 追赶：按 CATCHUP_SPEEDUP 倍速逐包排空（间隔缩短为 interval/SPEEDUP，
        // 净排空速率 = (SPEEDUP-1) × 正常速率）。刻意**不**一次性 drain()：整批
        // 背靠背打出只是把突发从发送端搬到接收端（8 个包挤在 <1ms 内到达会把
        // 接收端抖动估计瞬间抬高），与 pacing 的意图自相矛盾。加速排空既不留住
        // 积压，也保持到达间隔平滑。
        const auto now = std::chrono::steady_clock::now();
        if (now < next_send) {
            return;
        }
        if (send_one()) {
            catchup_drains_.fetch_add(1, std::memory_order_relaxed);
            paced_sends_.fetch_add(1, std::memory_order_relaxed);
            next_send = now
                + pacing_interval_ / config::DISPATCH_PACING_CATCHUP_SPEEDUP;
        }
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now < next_send) {
        return; // 未到发送时刻：帧留在队列里，burst 由此摊平
    }
    // 欠账钳制：时刻表最旧只允许落后 MAX_CATCHUP_SENDS 个间隔——长期队列空转
    // （无帧可发）不会把"信用"攒成恢复后的一次性大突发。
    const auto earliest = now - pacing_interval_ * config::DISPATCH_PACING_MAX_CATCHUP_SENDS;
    if (next_send < earliest) {
        next_send = earliest;
    }
    while (next_send <= now) {
        if (!send_one()) {
            break; // 队列空：时刻表留在原地，新帧到达时按欠账上限补发
        }
        next_send += pacing_interval_;
        paced_sends_.fetch_add(1, std::memory_order_relaxed);
    }
}

void AudioNetworkDispatcher::drain() noexcept
{
    while (send_one()) {
    }
}

bool AudioNetworkDispatcher::send_one() noexcept
{
    return queue_.consume_one([this](const audio::AudioFrame& frame) noexcept {
        try {
            // RTP 派生（无状态精确计算）：seq 取低 16 位；timestamp = seq × F
            // + offset（u64 回绕 well-defined，截断到 32 位后连续性不变）。
            const auto seq16 = static_cast<std::uint16_t>(frame.sequence & 0xFFFF);
            const auto timestamp = static_cast<std::uint32_t>(
                frame.sequence * queue_.frame_count() + rtp_timestamp_offset_);
            auto packet = std::make_shared<const std::vector<std::byte>>(
                net::NetworkFrame::audio(seq16, timestamp, rtp_ssrc_, frame.data).encode());
            if (packet->empty()) {
                encode_failures_.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            frames_encoded_.fetch_add(1, std::memory_order_relaxed);
            if (log_level_enabled(LogLevel::Trace)) {
                log_trace_fmt("AudioNetworkDispatcher encoded audio frame: seq={} bytes={}",
                    frame.sequence, frame.data.size());
            }
            const auto recipients = udp_.broadcast(std::move(packet));
            if (!recipients.has_value()) {
                dispatch_failures_.fetch_add(1, std::memory_order_relaxed);
            } else if (*recipients == 0) {
                frames_without_clients_.fetch_add(1, std::memory_order_relaxed);
            } else {
                frames_broadcast_.fetch_add(1, std::memory_order_relaxed);
            }
        } catch (...) {
            encode_failures_.fetch_add(1, std::memory_order_relaxed);
        }
    });
}

} // namespace aqua::runtime
