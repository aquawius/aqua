#include "aqua/runtime/audio_network_dispatcher.h"

#include "aqua/net/udp/network_frame.h"

#include <memory>
#include <system_error>
#include <vector>

namespace aqua::runtime {

AudioNetworkDispatcher::AudioNetworkDispatcher(
    audio::AudioFrameQueue& queue, net::UdpServer& udp) noexcept
    : queue_(queue)
    , udp_(udp)
{
}

AudioNetworkDispatcher::~AudioNetworkDispatcher()
{
    stop();
}

bool AudioNetworkDispatcher::start()
{
    if (worker_.joinable()) {
        return false;
    }
    stop_requested_.store(false, std::memory_order_release);
    try {
        worker_ = std::thread([this] { run(); });
        return true;
    } catch (const std::system_error&) {
        stop_requested_.store(true, std::memory_order_release);
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
}

void AudioNetworkDispatcher::notify_from_realtime() noexcept
{
    wake_generation_.fetch_add(1, std::memory_order_release);
    wake_generation_.notify_one();
}

void AudioNetworkDispatcher::run() noexcept
{
    auto observed = wake_generation_.load(std::memory_order_acquire);
    while (!stop_requested_.load(std::memory_order_acquire)) {
        drain();
        if (!queue_.empty()) {
            continue;
        }

        observed = wake_generation_.load(std::memory_order_acquire);
        if (queue_.empty() && !stop_requested_.load(std::memory_order_acquire)) {
            wake_generation_.wait(observed, std::memory_order_acquire);
        }
    }
    drain();
}

void AudioNetworkDispatcher::drain() noexcept
{
    while (queue_.consume_one([this](const audio::AudioFrame& frame) noexcept {
        try {
            auto packet = std::make_shared<const std::vector<std::byte>>(
                net::NetworkFrame::audio(frame.sequence, frame.data).encode());
            if (!packet->empty()) {
                frames_encoded_.fetch_add(1, std::memory_order_relaxed);
                const auto recipients = udp_.broadcast(std::move(packet));
                if (recipients > 0) {
                    frames_broadcast_.fetch_add(1, std::memory_order_relaxed);
                } else {
                    frames_without_clients_.fetch_add(1, std::memory_order_relaxed);
                }
            }
        } catch (...) {
            // Network worker failure is isolated from capture. The frame is lost, but
            // one allocation failure must never tear down the producer thread.
        }
    })) {
    }
}

} // namespace aqua::runtime
