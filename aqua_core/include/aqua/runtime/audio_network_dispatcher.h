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

    // capture producer 在每次 queue.push() 成功后调用。每次推进 generation；
    // 仅当 push 发布后仍判断本次 frame 可能需要唤醒 consumer 时 notify worker。
    // should_notify 是 producer 的唤醒提示，不是并发后的队列状态事实。
    // generation 解决 load→wait 竞态，notify_one() 唤醒已经阻塞的 worker。
    void publish_from_realtime(bool should_notify) noexcept;

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
    [[nodiscard]] std::uint64_t encode_failures() const noexcept
    {
        return encode_failures_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t dispatch_failures() const noexcept
    {
        return dispatch_failures_.load(std::memory_order_relaxed);
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
    std::atomic<std::uint64_t> encode_failures_ { 0 };
    std::atomic<std::uint64_t> dispatch_failures_ { 0 };
    std::thread worker_;
};

} // namespace aqua::runtime

#endif // AQUA_RUNTIME_AUDIO_NETWORK_DISPATCHER_H
