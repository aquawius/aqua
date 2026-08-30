#include "aqua/runtime/audio_network_dispatcher.h"

#include "aqua/net/udp/network_frame.h"
#include "aqua/logger/logger.h"

#include <memory>
#include <system_error>
#include <vector>

namespace aqua::runtime {

AudioNetworkDispatcher::AudioNetworkDispatcher(
    audio::AudioFrameQueue& queue, net::UdpServer& udp) noexcept
    : queue_(queue)
    , udp_(udp)
{
    log_debug_fmt("AudioNetworkDispatcher configured: queue_slots={} frame_count={} frame_bytes={} slot_bytes={}",
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
    log_debug("AudioNetworkDispatcher stopped");
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
    auto observed = wake_generation_.load(std::memory_order_acquire);
    while (!stop_requested_.load(std::memory_order_acquire)) {
        drain();
        if (!queue_.empty()) {
            continue;
        }

        observed = wake_generation_.load(std::memory_order_acquire);
        if (queue_.empty() && !stop_requested_.load(std::memory_order_acquire)) {
            wake_generation_.wait(observed, std::memory_order_acquire);
            worker_wakeups_.fetch_add(1, std::memory_order_relaxed);
        }
    }
    drain();
    log_debug("AudioNetworkDispatcher worker exited");
}

void AudioNetworkDispatcher::drain() noexcept
{
    while (queue_.consume_one([this](const audio::AudioFrame& frame) noexcept {
        try {
            auto packet = std::make_shared<const std::vector<std::byte>>(
                net::NetworkFrame::audio(frame.sequence, frame.data).encode());
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
    })) {
    }
}

} // namespace aqua::runtime
