#include "aqua/net/udp/network_frame.h"
#include "aqua/net/udp/udp_client.h"
#include "io_thread.h"

#include <asio.hpp>
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>
#include <vector>

namespace {

TEST(UdpClientLivenessTest, StartReceiveWithoutRemoteDoesNotOpenSocket)
{
    asio::io_context io;
    aqua::net::UdpClient client(io);

    EXPECT_FALSE(client.start_receive(4, [](std::uint64_t, std::span<const std::byte>) { }).has_value());
    EXPECT_FALSE(client.is_open());
}

TEST(UdpClientLivenessTest, TriggersAfterConsecutiveHeartbeatAckMisses)
{
    asio::io_context io;
    aqua::test::IoThread io_thread(io);

    asio::ip::udp::socket sink(io, asio::ip::udp::v4());
    sink.bind(asio::ip::udp::endpoint(asio::ip::udp::v4(), 0));
    const auto endpoint = sink.local_endpoint();

    aqua::net::UdpClient client(io);
    ASSERT_TRUE(client.set_remote("127.0.0.1", endpoint.port()).has_value());
    ASSERT_TRUE(client.start_receive(4, [](std::uint64_t, std::span<const std::byte>) { }).has_value());

    std::atomic<std::uint32_t> failures { 0 };
    const auto on_liveness_failure = [&failures](std::uint32_t misses) noexcept {
        failures.store(misses, std::memory_order_release);
    };
    ASSERT_TRUE(client.start_heartbeat(1234, std::chrono::milliseconds(20), on_liveness_failure));

    for (int i = 0; i < 100 && failures.load(std::memory_order_acquire) == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    EXPECT_GE(failures.load(std::memory_order_acquire), aqua::config::HEARTBEAT_HANDSHAKE_ACK_MISS_THRESHOLD);
    EXPECT_GE(client.consecutive_heartbeat_ack_misses(), aqua::config::HEARTBEAT_HANDSHAKE_ACK_MISS_THRESHOLD);
}

TEST(UdpClientLivenessTest, WrongSessionAckDoesNotResetLiveness)
{
    asio::io_context io;
    aqua::test::IoThread io_thread(io);

    asio::ip::udp::socket sink(io, asio::ip::udp::v4());
    sink.bind(asio::ip::udp::endpoint(asio::ip::udp::v4(), 0));
    const auto server_endpoint = sink.local_endpoint();

    aqua::net::UdpClient client(io);
    ASSERT_TRUE(client.set_remote("127.0.0.1", server_endpoint.port()).has_value());
    ASSERT_TRUE(client.start_receive(4, [](std::uint64_t, std::span<const std::byte>) { }).has_value());
    ASSERT_TRUE(client.start_heartbeat(777, std::chrono::milliseconds(20)));

    const auto client_port = client.local_endpoint().port();
    const auto client_target = asio::ip::udp::endpoint(asio::ip::address_v4::loopback(), client_port);

    const auto wrong_ack = aqua::net::NetworkFrame::heartbeat_ack(778).encode();
    sink.send_to(asio::buffer(wrong_ack), client_target);

    // 等待至少一个握手周期让 miss 计数推进；错误 session 的 ACK 不得被计为有效 ACK，
    // 因此 ack_count 保持 0，miss 计数持续累积。
    for (int i = 0; i < 100 && client.consecutive_heartbeat_ack_misses() == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    EXPECT_EQ(client.heartbeat_ack_count(), 0u);
    EXPECT_GE(client.consecutive_heartbeat_ack_misses(), 1u);
}

TEST(UdpClientLivenessTest, AckFromDifferentSourceWithCorrectSessionIsAccepted)
{
    asio::io_context io;
    aqua::test::IoThread io_thread(io);

    asio::ip::udp::socket server(io, asio::ip::udp::v4());
    server.bind(asio::ip::udp::endpoint(asio::ip::address_v4::loopback(), 0));
    const auto server_endpoint = server.local_endpoint();

    // ACK 来源与 client 的 remote（server_endpoint）不同：模拟 IPv6 隐私扩展/多地址。
    asio::ip::udp::socket ack_source(io, asio::ip::udp::v4());
    ack_source.bind(asio::ip::udp::endpoint(asio::ip::address_v4::loopback(), 0));

    aqua::net::UdpClient client(io);
    ASSERT_TRUE(client.set_remote("127.0.0.1", server_endpoint.port()).has_value());
    ASSERT_TRUE(client.start_receive(4, [](std::uint64_t, std::span<const std::byte>) { }).has_value());
    ASSERT_TRUE(client.start_heartbeat(9001, std::chrono::milliseconds(20)));

    const auto client_target = asio::ip::udp::endpoint(
        asio::ip::address_v4::loopback(), client.local_endpoint().port());
    const auto ack = aqua::net::NetworkFrame::heartbeat_ack(9001).encode();
    ack_source.send_to(asio::buffer(ack), client_target);

    // 来源与 remote 不同，但 session 正确：ACK 必须被接受（UDP endpoint discovery）。
    for (int i = 0; i < 100 && client.heartbeat_ack_count() == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    EXPECT_GE(client.heartbeat_ack_count(), 1u);
}

TEST(UdpClientLivenessTest, AckResetsConsecutiveMisses)
{
    asio::io_context io;
    aqua::test::IoThread io_thread(io);

    asio::ip::udp::socket sink(io, asio::ip::udp::v4());
    sink.bind(asio::ip::udp::endpoint(asio::ip::udp::v4(), 0));
    const auto server_endpoint = sink.local_endpoint();

    aqua::net::UdpClient client(io);
    ASSERT_TRUE(client.set_remote("127.0.0.1", server_endpoint.port()).has_value());
    ASSERT_TRUE(client.start_receive(4, [](std::uint64_t, std::span<const std::byte>) { }).has_value());

    ASSERT_TRUE(client.start_heartbeat(5678, std::chrono::milliseconds(50)));

    // client 绑定的是通配临时端口，local_endpoint() 返回 0.0.0.0:port；
    // 必须把 ACK 回发到该端口的 loopback 地址（0.0.0.0 不是合法的发送目标）。
    const auto client_port = client.local_endpoint().port();
    const auto client_target = asio::ip::udp::endpoint(asio::ip::address_v4::loopback(), client_port);

    const auto ack = aqua::net::NetworkFrame::heartbeat_ack(5678).encode();
    sink.send_to(asio::buffer(ack), client_target);

    for (int i = 0; i < 100 && client.heartbeat_ack_count() == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    ASSERT_GE(client.heartbeat_ack_count(), 1u);
    EXPECT_GE(client.heartbeat_ack_age_ms(), 0);

    // association 建立后稳态同样做 ACK 跟踪：再来一个 ACK，下一个稳态
    // tick（HEARTBEAT_INTERVAL = 1s）应看到并清零，不触发 liveness。
    // 允许 transition 竞态记 1 次（稳态 tick 可能抢在第二个 ACK 前跑）。
    sink.send_to(asio::buffer(ack), client_target);
    std::this_thread::sleep_for(std::chrono::milliseconds(1200));
    EXPECT_LE(client.consecutive_heartbeat_ack_misses(), 1u);
    EXPECT_FALSE(client.heartbeat_failed());
}

TEST(UdpClientLivenessTest, SetRemoteIsRejectedAfterReceiveStarts)
{
    asio::io_context io;
    asio::ip::udp::socket server(io, asio::ip::udp::v4());
    server.bind(asio::ip::udp::endpoint(asio::ip::address_v4::loopback(), 0));

    aqua::net::UdpClient client(io);
    ASSERT_TRUE(client.set_remote("127.0.0.1", server.local_endpoint().port()).has_value());
    ASSERT_TRUE(client.start_receive(4, [](std::uint64_t, std::span<const std::byte>) { }).has_value());
    EXPECT_FALSE(client.set_remote("127.0.0.1", server.local_endpoint().port()).has_value());
}

TEST(UdpClientLivenessTest, SetRemoteIsRejectedAfterHeartbeatStarts)
{
    asio::io_context io;
    aqua::test::IoThread io_thread(io);
    asio::ip::udp::socket server(io, asio::ip::udp::v4());
    server.bind(asio::ip::udp::endpoint(asio::ip::address_v4::loopback(), 0));

    aqua::net::UdpClient client(io);
    ASSERT_TRUE(client.set_remote("127.0.0.1", server.local_endpoint().port()).has_value());
    ASSERT_TRUE(client.start_heartbeat(123, std::chrono::milliseconds(50)));
    EXPECT_FALSE(client.set_remote("127.0.0.1", server.local_endpoint().port()).has_value());
}

TEST(UdpClientLivenessTest, StartHeartbeatWithoutRemoteDoesNotLockFutureStart)
{
    asio::io_context io;
    aqua::net::UdpClient client(io);

    EXPECT_FALSE(client.start_heartbeat(1234, std::chrono::milliseconds(20)));

    asio::ip::udp::socket server(io, asio::ip::udp::v4());
    server.bind(asio::ip::udp::endpoint(asio::ip::address_v4::loopback(), 0));
    ASSERT_TRUE(client.set_remote("127.0.0.1", server.local_endpoint().port()).has_value());
    EXPECT_TRUE(client.start_heartbeat(1234, std::chrono::milliseconds(20)));
}

TEST(UdpClientLivenessTest, LivenessFailureCallbackFiresOnlyOnce)
{
    asio::io_context io;
    aqua::test::IoThread io_thread(io);

    asio::ip::udp::socket sink(io, asio::ip::udp::v4());
    sink.bind(asio::ip::udp::endpoint(asio::ip::udp::v4(), 0));
    const auto server_endpoint = sink.local_endpoint();

    aqua::net::UdpClient client(io);
    ASSERT_TRUE(client.set_remote("127.0.0.1", server_endpoint.port()).has_value());
    ASSERT_TRUE(client.start_receive(4,
                          [](std::uint64_t, std::span<const std::byte>) noexcept { })
            .has_value());

    std::atomic<std::uint32_t> callback_count { 0 };
    std::atomic<std::uint32_t> callback_misses { 0 };
    const auto on_failure = [&callback_count, &callback_misses](std::uint32_t misses) noexcept {
        callback_count.fetch_add(1, std::memory_order_relaxed);
        callback_misses.store(misses, std::memory_order_release);
    };

    ASSERT_TRUE(client.start_heartbeat(
        0x1234u, std::chrono::milliseconds(20), on_failure));

    for (int i = 0; i < 200
        && callback_count.load(std::memory_order_acquire) == 0;
        ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    ASSERT_EQ(callback_count.load(std::memory_order_acquire), 1u);
    EXPECT_GE(callback_misses.load(std::memory_order_acquire),
        aqua::config::HEARTBEAT_HANDSHAKE_ACK_MISS_THRESHOLD);

    // 失败已锁存：之后再错过的 heartbeat 不得重复回调通知上层。
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    EXPECT_EQ(callback_count.load(std::memory_order_acquire), 1u);
}

TEST(UdpClientLivenessTest, SteadyStateMissesTriggerLiveness)
{
    // 稳态与握手期同模型：association 建立后若 server 不再回 ACK（路径死亡），
    // 连续 HEARTBEAT_ACK_MISS_THRESHOLD 个稳态周期即锁存失败。
    // 慢测试（约 5s：5 个 1s 稳态周期），生产节奏无法加速，接受。
    asio::io_context io;
    aqua::test::IoThread io_thread(io);

    asio::ip::udp::socket sink(io, asio::ip::udp::v4());
    sink.bind(asio::ip::udp::endpoint(asio::ip::udp::v4(), 0));
    const auto server_endpoint = sink.local_endpoint();

    aqua::net::UdpClient client(io);
    ASSERT_TRUE(client.set_remote("127.0.0.1", server_endpoint.port()).has_value());
    ASSERT_TRUE(client.start_receive(4,
                          [](std::uint64_t, std::span<const std::byte>) noexcept { })
            .has_value());
    ASSERT_TRUE(client.start_heartbeat(0x1234u, std::chrono::milliseconds(20)));

    const auto client_port = client.local_endpoint().port();
    const auto client_target = asio::ip::udp::endpoint(
        asio::ip::address_v4::loopback(), client_port);
    const auto ack = aqua::net::NetworkFrame::heartbeat_ack(0x1234u).encode();
    sink.send_to(asio::buffer(ack), client_target);
    ASSERT_TRUE([&] {
        for (int i = 0; i < 200 && client.heartbeat_ack_count() == 0; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return client.heartbeat_ack_count() > 0;
    }());

    // 此后零 ACK：轮询等失败锁存（上限 8s，余量给调度抖动）。
    bool failed = false;
    for (int i = 0; i < 160 && !(failed = client.heartbeat_failed()); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    EXPECT_TRUE(failed);
    EXPECT_GE(client.consecutive_heartbeat_ack_misses(), aqua::config::HEARTBEAT_ACK_MISS_THRESHOLD);
}

} // namespace
