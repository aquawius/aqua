#ifndef AQUA_RUNTIME_AUDIO_NETWORK_DISPATCHER_H
#define AQUA_RUNTIME_AUDIO_NETWORK_DISPATCHER_H

// Capture RT -> bounded handoff -> network worker.
//
// The dispatcher is the only component that crosses from the audio domain into the
// network protocol. It owns wire encoding, while UdpServer only handles session-aware
// datagram fan-out. No mutex, allocation or Asio submission occurs on the capture side.

#include "aqua/audio/audio_frame.h"
#include "aqua/audio/queue/audio_frame_queue.h"
#include "aqua/net/udp/udp_server.h"

#include <atomic>
#include <cstdint>
#include <thread>

namespace aqua::runtime {

class AudioNetworkDispatcher final {
public:
    AudioNetworkDispatcher(audio::AudioFrameQueue& queue, net::UdpServer& udp) noexcept;
    ~AudioNetworkDispatcher();

    AudioNetworkDispatcher(const AudioNetworkDispatcher&) = delete;
    AudioNetworkDispatcher& operator=(const AudioNetworkDispatcher&) = delete;

    bool start();
    void stop() noexcept;

    // 仅由 capture producer 在 queue.push() 报告 was_empty=true 时调用。
    void notify_from_realtime() noexcept;

    // 诊断：frame 已从 handoff queue 取出并交给 network layer 的次数。
    [[nodiscard]] std::uint64_t frames_encoded() const noexcept
    {
        return frames_encoded_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t dropped_frames() const noexcept
    {
        return queue_.dropped_frames();
    }
    [[nodiscard]] std::uint64_t frames_broadcast() const noexcept
    {
        return frames_broadcast_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t frames_without_clients() const noexcept
    {
        return frames_without_clients_.load(std::memory_order_relaxed);
    }

private:
    void run() noexcept;
    void drain() noexcept;

    audio::AudioFrameQueue& queue_;
    net::UdpServer& udp_;
    std::atomic<bool> stop_requested_ { false };
    std::atomic<std::uint64_t> wake_generation_ { 0 };
    std::atomic<std::uint64_t> frames_encoded_ { 0 };
    std::atomic<std::uint64_t> frames_broadcast_ { 0 };
    std::atomic<std::uint64_t> frames_without_clients_ { 0 };
    std::thread worker_;
};

} // namespace aqua::runtime

#endif // AQUA_RUNTIME_AUDIO_NETWORK_DISPATCHER_H
