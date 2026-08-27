// AudioNetworkDispatcher 的直接单测：验证 RT→network 的唤醒协议与 stop 的 drain 语义。
// 覆盖：
//   - publish_from_realtime() 能唤醒 worker 并完成 encode + broadcast；
//   - stop() 会把 stop 前已入队的帧全部 drain 掉再退出。

#include "aqua/runtime/audio_network_dispatcher.h"

#include "aqua/audio/queue/audio_frame_queue.h"
#include "aqua/net/udp/network_frame.h"
#include "aqua/net/udp/udp_server.h"
#include "aqua/session/session_manager.h"

#include <gtest/gtest.h>
#include <asio.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <thread>
#include <vector>

namespace {

using aqua::runtime::AudioNetworkDispatcher;

bool wait_until(std::function<bool()> predicate, std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::yield();
    }
    return predicate();
}

TEST(AudioNetworkDispatcherTest, NotifyFromRealtimeEncodesAndBroadcasts)
{
    asio::io_context ioc;

    // 接收端：独立 socket，绑定 127.0.0.1:0。
    asio::ip::udp::socket receiver(ioc);
    receiver.open(asio::ip::udp::v4());
    receiver.bind(asio::ip::udp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    const auto receiver_ep = receiver.local_endpoint();

    auto received = std::make_shared<std::vector<std::byte>>(256);
    auto received_len = std::make_shared<std::size_t>(0);
    asio::ip::udp::endpoint sender_ep;
    receiver.async_receive_from(asio::buffer(*received), sender_ep,
        [received, received_len](const asio::error_code& ec, std::size_t len) {
            if (!ec) {
                *received_len = len;
            }
        });

    auto sessions = std::make_shared<aqua::session::SessionManager>();
    aqua::net::UdpServer udp(ioc, sessions);
    ASSERT_TRUE(udp.bind("127.0.0.1", 0));
    ASSERT_TRUE(udp.start());

    const auto id = sessions->create_session();
    ASSERT_TRUE(id.has_value());
    ASSERT_TRUE(sessions->establish_session(*id, receiver_ep));

    aqua::audio::AudioFrameQueue queue(4, 4, 4);
    AudioNetworkDispatcher dispatcher(queue, udp);
    ASSERT_TRUE(dispatcher.start());

    std::array<std::byte, 16> bytes {};
    bytes.fill(std::byte { 0xAB }); // 4 帧 × 4 字节（F32LE 1ch）

    // 第一次 push 观察到 queue empty，因此应产生唤醒提示。
    const auto r = queue.push(aqua::audio::AudioFrame { 42, 4, bytes });
    ASSERT_TRUE(r.accepted);
    ASSERT_TRUE(r.should_notify);
    dispatcher.publish_from_realtime(r.should_notify);

    // worker 线程独立于 ioc，encode 计数不需要 ioc.run() 推进。
    ASSERT_TRUE(wait_until([&] { return dispatcher.frames_encoded() == 1u; },
        std::chrono::milliseconds(200)));

    // 真正把 datagram 发出去 + 收到，需要 ioc 跑发送泵与接收。
    ioc.run_for(std::chrono::milliseconds(100));

    ASSERT_GT(*received_len, 0u);
    const std::span<const std::byte> packet(received->data(), *received_len);
    const auto nf = aqua::net::NetworkFrame::decode(packet);
    ASSERT_TRUE(nf.has_value());
    EXPECT_EQ(nf->type(), aqua::net::PacketType::Audio);
    EXPECT_EQ(nf->sequence(), 42u);
    ASSERT_EQ(nf->payload().size(), 16u);
    EXPECT_EQ(std::to_integer<std::uint8_t>(nf->payload()[0]), 0xABu);

    dispatcher.stop();
    receiver.close();
}

TEST(AudioNetworkDispatcherTest, StopDrainsRemainingFrames)
{
    asio::io_context ioc;
    auto sessions = std::make_shared<aqua::session::SessionManager>();
    aqua::net::UdpServer udp(ioc, sessions);
    ASSERT_TRUE(udp.bind("127.0.0.1", 0));
    ASSERT_TRUE(udp.start());

    aqua::audio::AudioFrameQueue queue(8, 4, 4);
    AudioNetworkDispatcher dispatcher(queue, udp);
    ASSERT_TRUE(dispatcher.start());

    std::array<std::byte, 16> bytes {};
    bytes.fill(std::byte { 0x5A });

    constexpr std::uint32_t kCount = 5;
    for (std::uint32_t i = 0; i < kCount; ++i) {
        ASSERT_TRUE(queue.push(aqua::audio::AudioFrame { i, 4, bytes }).accepted);
    }

    // stop() join worker，worker 退出循环前执行最终 drain，把剩余帧全部编码。
    dispatcher.stop();

    EXPECT_EQ(dispatcher.frames_encoded(), kCount);
    EXPECT_EQ(dispatcher.frames_without_clients(), kCount);
    EXPECT_EQ(dispatcher.frames_broadcast(), 0u);
    udp.stop();
}

TEST(AudioNetworkDispatcherTest, ConditionalWakeKeepsWorkerLive)
{
    asio::io_context ioc;
    auto sessions = std::make_shared<aqua::session::SessionManager>();
    aqua::net::UdpServer udp(ioc, sessions);
    ASSERT_TRUE(udp.bind("127.0.0.1", 0));
    ASSERT_TRUE(udp.start());

    aqua::audio::AudioFrameQueue queue(8, 4, 4);
    AudioNetworkDispatcher dispatcher(queue, udp);
    ASSERT_TRUE(dispatcher.start());

    std::array<std::byte, 16> bytes {};
    constexpr std::uint32_t kRounds = 20000;
    for (std::uint32_t i = 0; i < kRounds; ++i) {
        // Publish a two-frame burst. The first push observes empty and requests the
        // wakeup; the second push observes non-empty and must not request another notify.
        auto first = queue.push(aqua::audio::AudioFrame { static_cast<std::uint64_t>(i) * 2, 4, bytes });
        ASSERT_TRUE(first.accepted);
        ASSERT_TRUE(first.should_notify);

        auto second = queue.push(aqua::audio::AudioFrame { static_cast<std::uint64_t>(i) * 2 + 1, 4, bytes });
        ASSERT_TRUE(second.accepted);
        ASSERT_FALSE(second.should_notify);

        dispatcher.publish_from_realtime(first.should_notify);

        ASSERT_TRUE(wait_until([&] { return queue.empty(); }, std::chrono::milliseconds(50)))
            << "dispatcher failed to drain round " << i;
    }

    dispatcher.stop();
    EXPECT_EQ(dispatcher.encode_failures(), 0u);
    EXPECT_EQ(queue.dropped_frames(), 0u);
    udp.stop();
}

} // namespace
