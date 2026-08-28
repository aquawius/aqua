#include "aqua/net/udp/udp_client.h"
#include "aqua/net/udp/network_frame.h"
#include "io_thread.h"

#include <asio.hpp>
#include <gtest/gtest.h>

#include <atomic>
#include <vector>
#include <chrono>
#include <cstdint>
#include <thread>

namespace {


TEST(UdpClientLivenessTest, StartReceiveWithoutRemoteDoesNotOpenSocket)
{
    asio::io_context io;
    aqua::net::UdpClient client(io);

    EXPECT_FALSE(client.start_receive(4, [](std::uint64_t, std::span<const std::byte>) {}));
    EXPECT_FALSE(client.is_open());
}

TEST(UdpClientLivenessTest, TriggersAfterConsecutiveHelloAckMisses)
{
    asio::io_context io;
    aqua::test::IoThread io_thread(io);

    asio::ip::udp::socket sink(io, asio::ip::udp::v4());
    sink.bind(asio::ip::udp::endpoint(asio::ip::udp::v4(), 0));
    const auto endpoint = sink.local_endpoint();

    aqua::net::UdpClient client(io);
    ASSERT_TRUE(client.set_remote("127.0.0.1", endpoint.port()));
    ASSERT_TRUE(client.start_receive(4, [](std::uint64_t, std::span<const std::byte>) {}));

    std::atomic<std::uint32_t> failures { 0 };
    const auto on_liveness_failure = [&failures](std::uint32_t misses) noexcept {
        failures.store(misses, std::memory_order_release);
    };
    ASSERT_TRUE(client.start_hello(1234, std::chrono::milliseconds(20), on_liveness_failure));

    for (int i = 0; i < 100 && failures.load(std::memory_order_acquire) == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    EXPECT_GE(failures.load(std::memory_order_acquire), aqua::config::HELLO_ACK_MISS_THRESHOLD);
    EXPECT_GE(client.consecutive_hello_ack_misses(), aqua::config::HELLO_ACK_MISS_THRESHOLD);
}

TEST(UdpClientLivenessTest, WrongSessionAckDoesNotResetLiveness)
{
    asio::io_context io;
    aqua::test::IoThread io_thread(io);

    asio::ip::udp::socket sink(io, asio::ip::udp::v4());
    sink.bind(asio::ip::udp::endpoint(asio::ip::udp::v4(), 0));
    const auto server_endpoint = sink.local_endpoint();

    aqua::net::UdpClient client(io);
    ASSERT_TRUE(client.set_remote("127.0.0.1", server_endpoint.port()));
    ASSERT_TRUE(client.start_receive(4, [](std::uint64_t, std::span<const std::byte>) {}));
    ASSERT_TRUE(client.start_hello(777, std::chrono::milliseconds(20)));

    const auto client_port = client.local_endpoint().port();
    const auto client_target =
        asio::ip::udp::endpoint(asio::ip::address_v4::loopback(), client_port);

    const auto wrong_ack = aqua::net::NetworkFrame::hello_ack(778).encode();
    sink.send_to(asio::buffer(wrong_ack), client_target);

    // 等待至少一个 HELLO 周期让 miss 计数推进；错误 session 的 ACK 不得被计为有效 ACK，
    // 因此 ack_count 保持 0，miss 计数持续累积。
    for (int i = 0; i < 100 && client.consecutive_hello_ack_misses() == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    EXPECT_EQ(client.hello_ack_count(), 0u);
    EXPECT_GE(client.consecutive_hello_ack_misses(), 1u);
}

TEST(UdpClientLivenessTest, AckFromUnexpectedSenderIsIgnored)
{
    asio::io_context io;
    aqua::test::IoThread io_thread(io);

    asio::ip::udp::socket server(io, asio::ip::udp::v4());
    server.bind(asio::ip::udp::endpoint(asio::ip::address_v4::loopback(), 0));
    const auto server_endpoint = server.local_endpoint();

    asio::ip::udp::socket attacker(io, asio::ip::udp::v4());
    attacker.bind(asio::ip::udp::endpoint(asio::ip::address_v4::loopback(), 0));

    aqua::net::UdpClient client(io);
    ASSERT_TRUE(client.set_remote("127.0.0.1", server_endpoint.port()));
    ASSERT_TRUE(client.start_receive(4, [](std::uint64_t, std::span<const std::byte>) {}));
    ASSERT_TRUE(client.start_hello(9001, std::chrono::milliseconds(20)));

    const auto client_target = asio::ip::udp::endpoint(
        asio::ip::address_v4::loopback(), client.local_endpoint().port());
    const auto ack = aqua::net::NetworkFrame::hello_ack(9001).encode();
    attacker.send_to(asio::buffer(ack), client_target);

    // 等待 HELLO miss 计数推进；来源非法的 ACK 必须被忽略（ack_count 保持 0），
    // 客户端因此持续累计 miss。
    for (int i = 0; i < 100 && client.consecutive_hello_ack_misses() == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    EXPECT_EQ(client.hello_ack_count(), 0u);
    EXPECT_GE(client.consecutive_hello_ack_misses(), 1u);
}

TEST(UdpClientLivenessTest, AckResetsConsecutiveMisses)
{
    asio::io_context io;
    aqua::test::IoThread io_thread(io);

    asio::ip::udp::socket sink(io, asio::ip::udp::v4());
    sink.bind(asio::ip::udp::endpoint(asio::ip::udp::v4(), 0));
    const auto server_endpoint = sink.local_endpoint();

    aqua::net::UdpClient client(io);
    ASSERT_TRUE(client.set_remote("127.0.0.1", server_endpoint.port()));
    ASSERT_TRUE(client.start_receive(4, [](std::uint64_t, std::span<const std::byte>) {}));

    ASSERT_TRUE(client.start_hello(5678, std::chrono::milliseconds(50)));

    // client 绑定的是通配临时端口，local_endpoint() 返回 0.0.0.0:port；
    // 必须把 ACK 回发到该端口的 loopback 地址（0.0.0.0 不是合法的发送目标）。
    const auto client_port = client.local_endpoint().port();
    const auto client_target = asio::ip::udp::endpoint(asio::ip::address_v4::loopback(), client_port);

    const auto ack = aqua::net::NetworkFrame::hello_ack(5678).encode();
    sink.send_to(asio::buffer(ack), client_target);

    for (int i = 0; i < 100 && client.hello_ack_count() == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    ASSERT_GE(client.hello_ack_count(), 1u);
    EXPECT_EQ(client.consecutive_hello_ack_misses(), 0u);
    EXPECT_GE(client.hello_ack_age_ms(), 0);

    for (int i = 0; i < 100 && client.consecutive_hello_ack_misses() == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    ASSERT_GE(client.consecutive_hello_ack_misses(), 1u);

    sink.send_to(asio::buffer(ack), client_target);
    for (int i = 0; i < 100 && client.consecutive_hello_ack_misses() != 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    EXPECT_EQ(client.consecutive_hello_ack_misses(), 0u);
}

TEST(UdpClientLivenessTest, SetRemoteIsRejectedAfterReceiveStarts)
{
    asio::io_context io;
    asio::ip::udp::socket server(io, asio::ip::udp::v4());
    server.bind(asio::ip::udp::endpoint(asio::ip::address_v4::loopback(), 0));

    aqua::net::UdpClient client(io);
    ASSERT_TRUE(client.set_remote("127.0.0.1", server.local_endpoint().port()));
    ASSERT_TRUE(client.start_receive(4, [](std::uint64_t, std::span<const std::byte>) {}));
    EXPECT_FALSE(client.set_remote("127.0.0.1", server.local_endpoint().port()));
}

TEST(UdpClientLivenessTest, SetRemoteIsRejectedAfterHelloStarts)
{
    asio::io_context io;
    aqua::test::IoThread io_thread(io);
    asio::ip::udp::socket server(io, asio::ip::udp::v4());
    server.bind(asio::ip::udp::endpoint(asio::ip::address_v4::loopback(), 0));

    aqua::net::UdpClient client(io);
    ASSERT_TRUE(client.set_remote("127.0.0.1", server.local_endpoint().port()));
    ASSERT_TRUE(client.start_hello(123, std::chrono::milliseconds(50)));
    EXPECT_FALSE(client.set_remote("127.0.0.1", server.local_endpoint().port()));
}

TEST(UdpClientLivenessTest, StartHelloWithoutRemoteDoesNotLockFutureStart)
{
    asio::io_context io;
    aqua::net::UdpClient client(io);

    EXPECT_FALSE(client.start_hello(1234, std::chrono::milliseconds(20)));

    asio::ip::udp::socket server(io, asio::ip::udp::v4());
    server.bind(asio::ip::udp::endpoint(asio::ip::address_v4::loopback(), 0));
    ASSERT_TRUE(client.set_remote("127.0.0.1", server.local_endpoint().port()));
    EXPECT_TRUE(client.start_hello(1234, std::chrono::milliseconds(20)));
}


TEST(UdpClientLivenessTest, LivenessFailureCallbackFiresOnlyOnce)
{
    asio::io_context io;
    aqua::test::IoThread io_thread(io);

    asio::ip::udp::socket sink(io, asio::ip::udp::v4());
    sink.bind(asio::ip::udp::endpoint(asio::ip::udp::v4(), 0));
    const auto server_endpoint = sink.local_endpoint();

    aqua::net::UdpClient client(io);
    ASSERT_TRUE(client.set_remote("127.0.0.1", server_endpoint.port()));
    ASSERT_TRUE(client.start_receive(4,
        [](std::uint64_t, std::span<const std::byte>) noexcept {}));

    std::atomic<std::uint32_t> callback_count { 0 };
    std::atomic<std::uint32_t> callback_misses { 0 };
    const auto on_failure = [&callback_count, &callback_misses](std::uint32_t misses) noexcept {
        callback_count.fetch_add(1, std::memory_order_relaxed);
        callback_misses.store(misses, std::memory_order_release);
    };

    ASSERT_TRUE(client.start_hello(
        0x1234u, std::chrono::milliseconds(20), on_failure));

    for (int i = 0; i < 200
        && callback_count.load(std::memory_order_acquire) == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    ASSERT_EQ(callback_count.load(std::memory_order_acquire), 1u);
    EXPECT_GE(callback_misses.load(std::memory_order_acquire),
        aqua::config::HELLO_ACK_MISS_THRESHOLD);

    // 失败已锁存：之后再错过的 HELLO 不得重复回调通知上层。
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    EXPECT_EQ(callback_count.load(std::memory_order_acquire), 1u);
}

} // namespace
