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

    for (int i = 0; i < 100 && client.hello_ack_count() != 0; ++i) {
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

    std::this_thread::sleep_for(std::chrono::milliseconds(30));
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

    // client binds a wildcard temporary port, so local_endpoint() is 0.0.0.0:port;
    // send the ACK back to loopback at that port (0.0.0.0 is not a valid send target).
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

} // namespace
