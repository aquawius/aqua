#include "aqua/net/udp/udp_config.h"
#include "aqua/net/udp/udp_transport.h"

#include "io_thread.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <future>
#include <algorithm>
#include <stdexcept>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;
using aqua::net::UdpTransport;
using aqua::test::IoThread;

std::vector<std::byte> bytes(std::initializer_list<unsigned char> values)
{
    std::vector<std::byte> out;
    out.reserve(values.size());
    for (const auto value : values) {
        out.push_back(static_cast<std::byte>(value));
    }
    return out;
}

TEST(UdpLoopbackTest, StartReceiveBeforeOpenFails)
{
    asio::io_context io;
    UdpTransport client(io);

    EXPECT_FALSE(client.start_receive([](const auto&, std::span<const std::byte>) {}));
    EXPECT_FALSE(client.is_open());
}

TEST(UdpLoopbackTest, ServerClientRoundTripIPv4)
{
    asio::io_context io;
    UdpTransport server(io);
    ASSERT_TRUE(server.bind("127.0.0.1", 0));
    const auto server_ep = server.socket_local_endpoint();
    ASSERT_TRUE(server_ep.address().is_v4());
    ASSERT_NE(server_ep.port(), 0);

    UdpTransport client(io);
    ASSERT_TRUE(client.set_remote(server_ep));

    const auto payload = bytes({ 0x01, 0x02, 0x7f, 0xff });
    std::promise<std::vector<std::byte>> received;
    auto future = received.get_future();
    std::atomic<bool> completed { false };

    ASSERT_TRUE(server.start_receive([&](const auto& sender, const auto data) {
        if (completed.exchange(true)) {
            return;
        }
        // 客户端绑定 0.0.0.0（通配），其本地 endpoint 地址不是真实来源地址；
        // 环回场景下 sender 一定是 loopback，端口即客户端实际使用的端口。
        EXPECT_TRUE(sender.address().is_loopback());
        EXPECT_EQ(sender.port(), client.socket_local_endpoint().port());
        received.set_value(std::vector<std::byte>(data.begin(), data.end()));
    }));

    IoThread thread(io);
    client.send(payload);

    ASSERT_EQ(future.wait_for(2s), std::future_status::ready);
    EXPECT_EQ(future.get(), payload);

    const auto stats = server.stats();
    EXPECT_EQ(stats.rx_packets, 1u);
    EXPECT_EQ(stats.rx_bytes, payload.size());
}

TEST(UdpLoopbackTest, ServerClientRoundTripIPv6)
{
    asio::io_context io;
    UdpTransport server(io);
    if (!server.bind("::1", 0)) {
        GTEST_SKIP() << "IPv6 loopback is unavailable on this host";
    }

    const auto server_ep = server.socket_local_endpoint();
    ASSERT_TRUE(server_ep.address().is_v6());
    ASSERT_NE(server_ep.port(), 0);

    UdpTransport client(io);
    ASSERT_TRUE(client.set_remote(server_ep));
    ASSERT_TRUE(client.socket_local_endpoint().address().is_v6());

    const auto payload = bytes({ 0x11, 0x22, 0x33, 0x44 });
    std::promise<std::vector<std::byte>> received;
    auto future = received.get_future();

    ASSERT_TRUE(server.start_receive([&](const auto&, const auto data) {
        received.set_value(std::vector<std::byte>(data.begin(), data.end()));
    }));

    IoThread thread(io);
    client.send(payload);

    ASSERT_EQ(future.wait_for(2s), std::future_status::ready);
    EXPECT_EQ(future.get(), payload);
}

TEST(UdpLoopbackTest, SharedPayloadCanBeSentToMultipleClients)
{
    asio::io_context io;
    UdpTransport server(io);
    ASSERT_TRUE(server.bind("127.0.0.1", 0));

    UdpTransport first(io);
    UdpTransport second(io);
    ASSERT_TRUE(first.set_remote(server.socket_local_endpoint()));
    ASSERT_TRUE(second.set_remote(server.socket_local_endpoint()));

    constexpr std::size_t expected = 2;
    std::mutex mutex;
    std::condition_variable cv;
    std::vector<std::vector<std::byte>> received;

    ASSERT_TRUE(server.start_receive([&](const auto&, const auto data) {
        {
            std::lock_guard lock(mutex);
            received.emplace_back(data.begin(), data.end());
        }
        cv.notify_one();
    }));

    auto shared = std::make_shared<const std::vector<std::byte>>(
        bytes({ 0xaa, 0xbb, 0xcc }));

    IoThread thread(io);
    first.send_shared(shared);
    second.send_shared(shared);

    std::unique_lock lock(mutex);
    ASSERT_TRUE(cv.wait_for(lock, 2s, [&] { return received.size() >= expected; }));
    ASSERT_EQ(received.size(), expected);
    EXPECT_EQ(received[0], *shared);
    EXPECT_EQ(received[1], *shared);
}

TEST(UdpLoopbackTest, DuplicateStartReceiveIsIgnored)
{
    asio::io_context io;
    UdpTransport server(io);
    ASSERT_TRUE(server.bind("127.0.0.1", 0));

    std::atomic<unsigned> first_calls { 0 };
    std::atomic<unsigned> second_calls { 0 };
    ASSERT_TRUE(server.start_receive([&](const auto&, const auto) { ++first_calls; }));
    ASSERT_TRUE(server.start_receive([&](const auto&, const auto) { ++second_calls; }));

    UdpTransport client(io);
    ASSERT_TRUE(client.set_remote(server.socket_local_endpoint()));

    // The first handler owns reception; the second handler must never replace it.
    IoThread thread(io);
    client.send(std::vector<std::byte> { std::byte { 0x01 } });
    std::this_thread::sleep_for(100ms);

    EXPECT_GE(first_calls.load(), 1u);
    EXPECT_EQ(second_calls.load(), 0u);
}

TEST(UdpLoopbackTest, StatisticsTrackTransmitAndReceive)
{
    asio::io_context io;
    UdpTransport server(io);
    ASSERT_TRUE(server.bind("127.0.0.1", 0));
    UdpTransport client(io);
    ASSERT_TRUE(client.set_remote(server.socket_local_endpoint()));

    std::promise<void> received;
    auto future = received.get_future();
    ASSERT_TRUE(server.start_receive([&](const auto&, const auto) { received.set_value(); }));

    const auto payload = bytes({ 1, 2, 3, 4, 5 });
    IoThread thread(io);
    client.send(payload);

    ASSERT_EQ(future.wait_for(2s), std::future_status::ready);

    const auto server_stats = server.stats();
    EXPECT_EQ(server_stats.rx_packets, 1u);
    EXPECT_EQ(server_stats.rx_bytes, payload.size());

    // 发送完成回调在 strand 上异步执行，可能晚于对端收包；轮询等待统计就绪。
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (client.stats().tx_packets == 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
    }
    const auto client_stats = client.stats();
    EXPECT_EQ(client_stats.tx_packets, 1u);
    EXPECT_EQ(client_stats.tx_bytes, payload.size());
}

TEST(UdpLoopbackTest, StopIsIdempotentAndPreventsFurtherWork)
{
    asio::io_context io;
    UdpTransport server(io);
    ASSERT_TRUE(server.bind("127.0.0.1", 0));
    ASSERT_TRUE(server.is_open());

    ASSERT_TRUE(server.start_receive([](const auto&, const auto) {}));
    server.stop();
    server.stop();

    EXPECT_FALSE(server.is_open());
    EXPECT_FALSE(server.start_receive([](const auto&, const auto) {}));
}

TEST(UdpLoopbackTest, ClientRejectsZeroPortAndMissingRemote)
{
    asio::io_context io;
    UdpTransport client(io);
    EXPECT_FALSE(client.set_remote("127.0.0.1", 0));
    EXPECT_FALSE(client.has_remote());

    const std::byte value { 0x42 };
    client.send(std::span<const std::byte>(&value, 1));
    SUCCEED(); // send without remote must be a safe no-op.
}

TEST(UdpLoopbackTest, ClientAutomaticallySelectsIPv6Socket)
{
    asio::io_context io;
    UdpTransport client(io);
    if (!client.set_remote("::1", 9)) {
        GTEST_SKIP() << "IPv6 loopback is unavailable on this host";
    }

    ASSERT_TRUE(client.is_open());
    EXPECT_TRUE(client.socket_local_endpoint().address().is_v6());
    EXPECT_TRUE(client.remote_endpoint().address().is_v6());
}

TEST(UdpLoopbackTest, ClientRejectsChangingAddressFamilyAfterOpen)
{
    asio::io_context io;
    UdpTransport client(io);
    ASSERT_TRUE(client.open());
    ASSERT_TRUE(client.socket_local_endpoint().address().is_v4());

    EXPECT_FALSE(client.set_remote(asio::ip::udp::endpoint(
        asio::ip::make_address("::1"), 9999)));
}

TEST(UdpLoopbackTest, SharedSendQueueDropsOldestWhenIoIsDelayed)
{
    asio::io_context io;
    UdpTransport server(io);
    ASSERT_TRUE(server.bind("127.0.0.1", 0));

    UdpTransport client(io);
    ASSERT_TRUE(client.set_remote(server.socket_local_endpoint()));

    // Block io_context before queued transport handlers are allowed to run. This makes
    // the transport's user-space bounded queue deterministic rather than depending on
    // scheduler timing.
    std::promise<void> release;
    auto release_future = release.get_future().share();
    asio::post(io, [release_future] { release_future.wait(); });

    IoThread thread(io);

    const auto payload = std::make_shared<const std::vector<std::byte>>(bytes({ 0x5a }));
    for (std::size_t i = 0; i < aqua::config::UDP_MAX_QUEUED_DATAGRAMS + 8; ++i) {
        client.send_shared(payload);
    }

    // Allow the blocked io_context to drain the posted send operations.
    release.set_value();
    std::this_thread::sleep_for(50ms);

    const auto stats = client.stats();
    EXPECT_GE(stats.tx_dropped, 1u);
}

} // namespace


// 补充用例：server->client 方向、边界 no-op 与生命周期安全。
// 既有用例只覆盖了 client->server 方向；server 的发送路径（send_copy/send_shared）
// 与 client 的接收路径需要成对验证（对应 HELLO/ACK 回包与音频广播的逆向链路）。
namespace {

TEST(UdpLoopbackTest, ServerRepliesToClientSenderEndpoint)
{
    asio::io_context io;
    UdpTransport server(io);
    ASSERT_TRUE(server.bind("127.0.0.1", 0));

    UdpTransport client(io);
    ASSERT_TRUE(client.set_remote(server.socket_local_endpoint()));

    // 与真实协议一致：客户端先发 HELLO，server 记录数据包的来源 endpoint
    //（NAT 映射后的地址），再向该 endpoint 回发 ACK。客户端绑定 0.0.0.0，
    // 其本地 endpoint 地址（通配）不能作为目的地址，必须用来源 endpoint。
    std::promise<asio::ip::udp::endpoint> got_hello;
    auto hello_future = got_hello.get_future();
    ASSERT_TRUE(server.start_receive([&](const auto& sender, const auto) {
        got_hello.set_value(sender);
    }));

    std::promise<std::vector<std::byte>> received;
    auto reply_future = received.get_future();
    ASSERT_TRUE(client.start_receive([&](const auto&, const auto data) {
        received.set_value(std::vector<std::byte>(data.begin(), data.end()));
    }));

    IoThread thread(io);
    const auto hello = bytes({ 0x01 });
    client.send(hello);

    ASSERT_EQ(hello_future.wait_for(2s), std::future_status::ready);
    const auto client_ep = hello_future.get();
    EXPECT_TRUE(client_ep.address().is_loopback());

    // server 向 HELLO 的来源 endpoint 回发 ACK（拷贝语义，server->client 方向）。
    const auto ack = bytes({ 0xde, 0xad, 0xbe, 0xef });
    server.send_to(client_ep, ack);

    ASSERT_EQ(reply_future.wait_for(2s), std::future_status::ready);
    EXPECT_EQ(reply_future.get(), ack);

    // server 发送统计也在 strand 上异步累加，轮询等待。
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (server.stats().tx_packets == 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
    }
    const auto server_stats = server.stats();
    EXPECT_EQ(server_stats.tx_packets, 1u);
    EXPECT_EQ(server_stats.tx_bytes, ack.size());
    EXPECT_EQ(client.stats().rx_packets, 1u);
}

TEST(UdpLoopbackTest, SendSharedNullIsNoOp)
{
    asio::io_context io;
    UdpTransport server(io);
    ASSERT_TRUE(server.bind("127.0.0.1", 0));
    UdpTransport client(io);
    ASSERT_TRUE(client.set_remote(server.socket_local_endpoint()));

    IoThread thread(io);
    // 空缓冲必须安全无操作（send_to_shared 对 !data 直接返回）。
    client.send_shared(nullptr);
    std::this_thread::sleep_for(50ms);

    const auto stats = client.stats();
    EXPECT_EQ(stats.tx_packets, 0u);
    EXPECT_EQ(stats.tx_queue_depth, 0u);
}

TEST(UdpLoopbackTest, SendAfterStopIsNoOp)
{
    asio::io_context io;
    UdpTransport server(io);
    ASSERT_TRUE(server.bind("127.0.0.1", 0));
    UdpTransport client(io);
    ASSERT_TRUE(client.set_remote(server.socket_local_endpoint()));

    client.stop();
    IoThread thread(io);
    // 已停止的 transport 不应投递发送，也不能崩溃（stopped 标志短路）。
    client.send(bytes({ 0x01 }));
    std::this_thread::sleep_for(50ms);

    const auto stats = client.stats();
    EXPECT_EQ(stats.tx_packets, 0u);
    EXPECT_EQ(stats.tx_queue_depth, 0u);
}

TEST(UdpLoopbackTest, SetRemoteAcceptsBracketedIPv6String)
{
    asio::io_context io;
    UdpTransport client(io);
    // 字符串版支持方括号 IPv6（parse_ip_address 会剥括号再解析）。
    if (!client.set_remote("[::1]", 9)) {
        GTEST_SKIP() << "IPv6 loopback is unavailable on this host";
    }

    EXPECT_TRUE(client.is_open());
    EXPECT_TRUE(client.remote_endpoint().address().is_v6());
    EXPECT_EQ(client.remote_endpoint().address().to_string(), "::1");
}

} // namespace

// Extra lifecycle/edge tests kept separate from the round-trip cases above.
namespace {

TEST(UdpLoopbackTest, ClientOpenIsIdempotent)
{
    asio::io_context io;
    UdpTransport client(io);
    ASSERT_TRUE(client.open());
    const auto first = client.socket_local_endpoint();
    ASSERT_TRUE(client.open());
    const auto second = client.socket_local_endpoint();
    EXPECT_EQ(first, second);
}

TEST(UdpLoopbackTest, ServerBindIsIdempotentForSameEndpoint)
{
    asio::io_context io;
    UdpTransport server(io);
    ASSERT_TRUE(server.bind("127.0.0.1", 0));
    const auto endpoint = server.socket_local_endpoint();
    ASSERT_TRUE(server.bind("127.0.0.1", endpoint.port()));
    EXPECT_EQ(server.socket_local_endpoint(), endpoint);
}

TEST(UdpLoopbackTest, ServerRejectsDifferentEndpointAfterBind)
{
    asio::io_context io;
    UdpTransport server(io);
    ASSERT_TRUE(server.bind("127.0.0.1", 0));
    const auto endpoint = server.socket_local_endpoint();
    EXPECT_FALSE(server.bind("127.0.0.1", static_cast<std::uint16_t>(endpoint.port() + 1)));
}

TEST(UdpLoopbackTest, StopPreventsReopen)
{
    asio::io_context io;
    UdpTransport server(io);
    ASSERT_TRUE(server.bind("127.0.0.1", 0));
    server.stop();
    EXPECT_FALSE(server.bind("127.0.0.1", 0));
}

TEST(UdpLoopbackTest, SendCopyOwnsItsPayloadBeforeIoRuns)
{
    asio::io_context io;
    UdpTransport server(io);
    ASSERT_TRUE(server.bind("127.0.0.1", 0));
    UdpTransport client(io);
    ASSERT_TRUE(client.set_remote(server.socket_local_endpoint()));

    const auto expected = bytes({ 9, 8, 7, 6 });
    std::promise<std::vector<std::byte>> received;
    auto future = received.get_future();
    ASSERT_TRUE(server.start_receive([&](const auto&, const auto data) {
        received.set_value(std::vector<std::byte>(data.begin(), data.end()));
    }));

    std::vector<std::byte> mutable_payload = expected;
    client.send(mutable_payload);
    std::fill(mutable_payload.begin(), mutable_payload.end(), std::byte { 0 });

    IoThread thread(io);
    ASSERT_EQ(future.wait_for(2s), std::future_status::ready);
    EXPECT_EQ(future.get(), expected);
}

TEST(UdpLoopbackTest, ReceiveHandlerExceptionDoesNotStopReceiveLoop)
{
    asio::io_context io;
    UdpTransport server(io);
    ASSERT_TRUE(server.bind("127.0.0.1", 0));
    UdpTransport client(io);
    ASSERT_TRUE(client.set_remote(server.socket_local_endpoint()));

    std::atomic<unsigned> calls { 0 };
    std::promise<void> second_packet;
    auto future = second_packet.get_future();
    ASSERT_TRUE(server.start_receive([&](const auto&, const auto) {
        if (calls.fetch_add(1) == 0) {
            throw std::runtime_error("test exception");
        }
        second_packet.set_value();
    }));

    IoThread thread(io);
    const auto payload = bytes({ 0x01 });
    client.send(payload);
    client.send(payload);

    ASSERT_EQ(future.wait_for(2s), std::future_status::ready);
    EXPECT_GE(calls.load(), 2u);
}

} // namespace
