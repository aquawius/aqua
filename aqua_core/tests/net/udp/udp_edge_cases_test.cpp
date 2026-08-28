#include "aqua/net/udp/udp_config.h"
#include "aqua/net/udp/udp_transport.h"

#include "io_thread.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <future>
#include <mutex>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;
using aqua::net::UdpTransport;
using aqua::test::IoThread;

std::vector<std::byte> make_payload(std::uint8_t value)
{
    return { static_cast<std::byte>(value) };
}

TEST(UdpEdgeTest, SendSharedKeepsPayloadAliveAfterCallerReset)
{
    asio::io_context io;
    UdpTransport server(io);
    ASSERT_TRUE(server.bind("127.0.0.1", 0));

    UdpTransport client(io);
    ASSERT_TRUE(client.set_remote(server.local_endpoint()));

    const auto expected = make_payload(0x7b);
    std::promise<std::vector<std::byte>> received;
    auto future = received.get_future();
    ASSERT_TRUE(server.start_receive([&](const auto&, const auto data) {
        received.set_value(std::vector<std::byte>(data.begin(), data.end()));
    }));

    auto payload = std::make_shared<const std::vector<std::byte>>(expected);
    client.send_shared(payload);
    payload.reset();

    IoThread thread(io);
    ASSERT_EQ(future.wait_for(2s), std::future_status::ready);
    EXPECT_EQ(future.get(), expected);
}

TEST(UdpEdgeTest, StopCancelsPendingReceive)
{
    asio::io_context io;
    UdpTransport server(io);
    ASSERT_TRUE(server.bind("127.0.0.1", 0));

    std::atomic<unsigned> callbacks { 0 };
    ASSERT_TRUE(server.start_receive([&](const auto&, const auto) {
        callbacks.fetch_add(1, std::memory_order_relaxed);
    }));

    IoThread thread(io);
    std::this_thread::sleep_for(10ms);
    server.stop();
    const auto before = callbacks.load(std::memory_order_relaxed);
    std::this_thread::sleep_for(50ms);

    EXPECT_EQ(callbacks.load(std::memory_order_relaxed), before);
    EXPECT_FALSE(server.is_open());
    EXPECT_FALSE(server.start_receive([](const auto&, const auto) {}));
}

TEST(UdpEdgeTest, StopDuringQueuedSendDrainsWithoutCrash)
{
    asio::io_context io;
    UdpTransport server(io);
    ASSERT_TRUE(server.bind("127.0.0.1", 0));

    UdpTransport client(io);
    ASSERT_TRUE(client.set_remote(server.local_endpoint()));

    IoThread thread(io);
    const auto payload = make_payload(0x55);
    for (int i = 0; i < 128; ++i) {
        client.send(payload);
    }
    client.stop();

    std::this_thread::sleep_for(50ms);
    EXPECT_FALSE(client.is_open());
    const auto stats = client.stats();
    EXPECT_EQ(stats.tx_queue_depth, 0u);
    EXPECT_EQ(stats.tx_errors, 0u); // normal shutdown cancellation is not a send error
}

TEST(UdpEdgeTest, ConcurrentSendAndStopIsSafe)
{
    asio::io_context io;
    UdpTransport server(io);
    ASSERT_TRUE(server.bind("127.0.0.1", 0));

    UdpTransport client(io);
    ASSERT_TRUE(client.set_remote(server.local_endpoint()));

    IoThread thread(io);
    const auto payload = make_payload(0x3c);

    std::atomic<bool> running { true };
    std::thread producer([&] {
        while (running.load(std::memory_order_relaxed)) {
            client.send(payload);
        }
    });

    std::this_thread::sleep_for(10ms);
    client.stop();
    running.store(false, std::memory_order_relaxed);
    producer.join();

    std::this_thread::sleep_for(20ms);
    EXPECT_FALSE(client.is_open());
    EXPECT_EQ(client.stats().tx_queue_depth, 0u);
}

TEST(UdpEdgeTest, ServerBroadcastsSharedPayloadToMultipleClients)
{
    asio::io_context io;
    UdpTransport server(io);
    ASSERT_TRUE(server.bind("127.0.0.1", 0));

    UdpTransport first(io);
    UdpTransport second(io);
    ASSERT_TRUE(first.set_remote(server.local_endpoint()));
    ASSERT_TRUE(second.set_remote(server.local_endpoint()));

    std::promise<asio::ip::udp::endpoint> first_sender;
    std::promise<asio::ip::udp::endpoint> second_sender;
    auto first_sender_future = first_sender.get_future();
    auto second_sender_future = second_sender.get_future();

    std::promise<std::vector<std::byte>> first_reply;
    std::promise<std::vector<std::byte>> second_reply;
    auto first_reply_future = first_reply.get_future();
    auto second_reply_future = second_reply.get_future();

    std::mutex mutex;
    std::vector<asio::ip::udp::endpoint> senders;
    ASSERT_TRUE(server.start_receive([&](const auto& sender, const auto) {
        {
            std::lock_guard lock(mutex);
            senders.push_back(sender);
        }
        try {
            if (sender.port() == first.local_endpoint().port()) {
                first_sender.set_value(sender);
            } else if (sender.port() == second.local_endpoint().port()) {
                second_sender.set_value(sender);
            }
        } catch (...) {
        }
    }));

    ASSERT_TRUE(first.start_receive([&](const auto&, const auto data) {
        try {
            first_reply.set_value(std::vector<std::byte>(data.begin(), data.end()));
        } catch (...) {
        }
    }));
    ASSERT_TRUE(second.start_receive([&](const auto&, const auto data) {
        try {
            second_reply.set_value(std::vector<std::byte>(data.begin(), data.end()));
        } catch (...) {
        }
    }));

    // Each client must first send a HELLO-like datagram so the server learns the
    // real source endpoint. The client's local address is 0.0.0.0 and must not be
    // used as a destination for the server response.
    first.send(make_payload(0x01));
    second.send(make_payload(0x02));

    IoThread thread(io);
    ASSERT_EQ(first_sender_future.wait_for(2s), std::future_status::ready);
    ASSERT_EQ(second_sender_future.wait_for(2s), std::future_status::ready);

    const auto first_ep = first_sender_future.get();
    const auto second_ep = second_sender_future.get();
    const auto payload = std::make_shared<const std::vector<std::byte>>(
        std::vector<std::byte> { std::byte { 0xa1 }, std::byte { 0xb2 } });

    server.send_to_shared(first_ep, payload);
    server.send_to_shared(second_ep, payload);

    ASSERT_EQ(first_reply_future.wait_for(2s), std::future_status::ready);
    ASSERT_EQ(second_reply_future.wait_for(2s), std::future_status::ready);
    EXPECT_EQ(first_reply_future.get(), *payload);
    EXPECT_EQ(second_reply_future.get(), *payload);

    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (server.stats().tx_packets < 2 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
    }
    EXPECT_EQ(server.stats().tx_packets, 2u);
    EXPECT_EQ(server.stats().tx_bytes, payload->size() * 2);
}

TEST(UdpEdgeTest, QueueOverflowDropsOldDataButKeepsNewestQueuedDatagram)
{
    asio::io_context io;
    UdpTransport server(io);
    ASSERT_TRUE(server.bind("127.0.0.1", 0));

    UdpTransport client(io);
    ASSERT_TRUE(client.set_remote(server.local_endpoint()));

    std::promise<std::byte> newest_received;
    auto newest_future = newest_received.get_future();
    constexpr std::uint8_t newest_value = 0xee;

    ASSERT_TRUE(server.start_receive([&](const auto&, const auto data) {
        if (data.size() == 1 && data[0] == static_cast<std::byte>(newest_value)) {
            try {
                newest_received.set_value(data[0]);
            } catch (...) {
            }
        }
    }));

    // Block the transport strand before enqueueing. The newest datagram must survive
    // overflow because the queue policy is explicitly drop-oldest.
    std::promise<void> release;
    auto release_future = release.get_future().share();
    asio::post(io, [release_future] { release_future.wait(); });
    IoThread thread(io);

    const auto total = aqua::config::UDP_MAX_QUEUED_DATAGRAMS + 16;
    for (std::size_t i = 0; i < total; ++i) {
        client.send(make_payload(static_cast<std::uint8_t>((i + 1) & 0xffu)));
    }
    client.send(make_payload(newest_value));

    // The queue itself is bounded; executor backlog must not grow once the queue is full.
    EXPECT_EQ(client.stats().tx_queue_depth, aqua::config::UDP_MAX_QUEUED_DATAGRAMS);

    release.set_value();

    ASSERT_EQ(newest_future.wait_for(2s), std::future_status::ready);
    EXPECT_EQ(newest_future.get(), static_cast<std::byte>(newest_value));
    EXPECT_GE(client.stats().tx_dropped, 1u);
}

} // namespace
