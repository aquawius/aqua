#include "aqua/runtime/audio_network_dispatcher.h"

#include "aqua/logger/logger.h"
#include "aqua/net/udp/network_frame.h"
#include "aqua/runtime/runtime_config.h"

#include <chrono>

#include <memory>
#include <system_error>
#include <vector>

namespace aqua::runtime {

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
    stop_requested_.store(false, std::memory_order_release);
    try {
        worker_ = std::thread([this] { run(); });
        log_debug("AudioNetworkDispatcher started");
        return true;
    } catch (const std::system_error& e) {
        stop_requested_.store(true, std::memory_order_release);
        log_error_fmt("AudioNetworkDispatcher: failed to start worker thread: {}", format_exception_message(e));
        return false;
    } catch (...) {
        stop_requested_.store(true, std::memory_order_release);
        log_error("AudioNetworkDispatcher: failed to start worker thread");
        return false;
    }
}

void AudioNetworkDispatcher::stop() noexcept
{
    stop_requested_.store(true, std::memory_order_release);
    wake_generation_.fetch_add(1, std::memory_order_release);
    wake_generation_.notify_one();
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

void AudioNetworkDispatcher::run() noexcept
{
    log_debug("AudioNetworkDispatcher worker entered");
    const bool paced = pacing_interval_ > std::chrono::nanoseconds::zero();
    auto next_send = std::chrono::steady_clock::now();
    while (!stop_requested_.load(std::memory_order_acquire)) {
        if (paced) {
            drain_paced(next_send);
        } else {
            drain();
        }
        if (!queue_.empty()) {
            if (paced) {
                // 队列未空但未到发送时刻：睡到下一时刻（间隔有界，stop 至多
                // 晚一个 packet 周期被观察到；期间到达的新帧留在队列里摊平）。
                const auto now = std::chrono::steady_clock::now();
                if (now < next_send) {
                    std::this_thread::sleep_until(next_send);
                }
            }
            continue;
        }

        const auto observed = wake_generation_.load(std::memory_order_acquire);
        if (queue_.empty() && !stop_requested_.load(std::memory_order_acquire)) {
            wake_generation_.wait(observed, std::memory_order_acquire);
            worker_wakeups_.fetch_add(1, std::memory_order_relaxed);
        }
    }
    drain();
    log_debug("AudioNetworkDispatcher worker exited");
}

// pacing 出队：队列深度达到追赶阈值说明 worker 被调度饿死过（或长时间无
// client 后堆积），一次性清空后重新起表；稳态 burst 深度低于阈值，不会误入。
void AudioNetworkDispatcher::drain_paced(
    std::chrono::steady_clock::time_point& next_send) noexcept
{
    if (queue_.size_slots() >= config::DISPATCH_PACING_CATCHUP_DEPTH_SLOTS) {
        catchup_drains_.fetch_add(1, std::memory_order_relaxed);
        drain();
        next_send = std::chrono::steady_clock::now() + pacing_interval_;
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now < next_send) {
        return; // 未到发送时刻：帧留在队列里，burst 由此摊平
    }
    if (send_one()) {
        next_send = now + pacing_interval_;
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
