#ifndef AQUA_RUNTIME_AUDIO_NETWORK_DISPATCHER_H
#define AQUA_RUNTIME_AUDIO_NETWORK_DISPATCHER_H

// Capture RT -> 有界交接 -> network worker。
//
// dispatcher 是唯一跨越「audio 域 → 网络协议」的组件：它负责 wire 编码，
// UdpServer 只做按 session 感知的 datagram 扇出。
// 采集侧不发生任何互斥、堆分配或 Asio 提交。

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

    // RTP 流参数（server 每 run 一组，由 ServerRuntime 在 start() 前一次性设定；
    // worker 启动后的 happens-before 由线程创建保证，运行期不再修改）：
    // ssrc = 发送流随机 ID；timestamp_offset = 首帧时间戳随机偏移（RFC 3550）。
    // 时间戳按 timestamp = seq × F + offset 精确派生（F 固定，packetizer 序号
    // 单调），无需跨线程状态；seq 取低 16 位截断（回绕由接收端展开）。
    void set_rtp_params(std::uint32_t ssrc, std::uint32_t timestamp_offset) noexcept
    {
        rtp_ssrc_ = ssrc;
        rtp_timestamp_offset_ = timestamp_offset;
    }

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
        // 口径说明：此处计的是 handoff queue 满丢（与 queue_dropped_frames 同源），
        // 不是 dispatcher 自身编码/发送失败。别名保留供 server 诊断快照直读。
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
    [[nodiscard]] std::uint64_t published_frames() const noexcept
    {
        return published_frames_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t worker_wakeups() const noexcept
    {
        return worker_wakeups_.load(std::memory_order_relaxed);
    }

private:
    void run() noexcept;
    void drain() noexcept;

    audio::AudioFrameQueue& queue_;
    net::UdpServer& udp_;
    std::uint32_t rtp_ssrc_ = 0;
    std::uint32_t rtp_timestamp_offset_ = 0;
    std::atomic<bool> stop_requested_ { false };
    std::atomic<std::uint64_t> wake_generation_ { 0 };
    std::atomic<std::uint64_t> published_frames_ { 0 };
    std::atomic<std::uint64_t> worker_wakeups_ { 0 };
    std::atomic<std::uint64_t> frames_encoded_ { 0 };
    std::atomic<std::uint64_t> frames_broadcast_ { 0 };
    std::atomic<std::uint64_t> frames_without_clients_ { 0 };
    std::atomic<std::uint64_t> encode_failures_ { 0 };
    std::atomic<std::uint64_t> dispatch_failures_ { 0 };
    std::thread worker_;
};

} // namespace aqua::runtime

#endif // AQUA_RUNTIME_AUDIO_NETWORK_DISPATCHER_H
