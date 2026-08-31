// UDP 数据面协议层测试（UdpServer / UdpClient）：
// HELLO 握手（establish + Ack 回复）、未知 session / malformed 包忽略、
// 音频广播（UdpServer::broadcast → UdpClient datagram callback）、
// client 对 Hello/HelloAck 的内部过滤。
// 传输层收发细节见 udp_loopback_test.cpp / udp_edge_cases_test.cpp。

#include "aqua/net/udp/network_frame.h"
#include "aqua/net/udp/udp_client.h"
#include "aqua/net/udp/udp_server.h"
#include "aqua/session/session_manager.h"

#include "io_thread.h"

#include <asio.hpp>
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <span>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;
using aqua::net::UdpClient;
using aqua::net::UdpServer;
using aqua::net::UdpTransport;
using aqua::session::SessionManager;
using aqua::test::IoThread;

constexpr std::uint32_t kFramesPerSlot = 4;
constexpr std::uint32_t kFrameBytes = 4;
constexpr std::size_t kPayloadBytes = 16; // 4 帧 × 4 字节（PCM_F32LE 1ch）

// 轮询等待条件成立（deadline 内），避免 sleep 硬编码。
bool wait_for(const std::function<bool()>& predicate,
    std::chrono::milliseconds timeout = 2s)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(5ms);
    }
    return predicate();
}

std::vector<std::byte> make_payload(std::uint8_t fill)
{
    return std::vector<std::byte>(kPayloadBytes, static_cast<std::byte>(fill));
}

TEST(UdpProtocolTest, HelloHandshakeEstablishesSession)
{
    asio::io_context io;
    auto sessions = std::make_shared<SessionManager>();
    const auto id = sessions->create_session();
    ASSERT_TRUE(id.has_value());

    UdpServer server(io, sessions);
    ASSERT_TRUE(server.bind("127.0.0.1", 0));
    ASSERT_TRUE(server.start());

    UdpClient client(io);
    ASSERT_TRUE(client.set_remote("127.0.0.1", server.local_endpoint().port()));
    ASSERT_TRUE(client.start_receive(kFramesPerSlot * kFrameBytes, [](std::uint64_t, std::span<const std::byte>) { }));
    client.start_hello(*id, 20ms);

    IoThread thread(io);
    // HELLO 周期发送 → server establish → session Connected。
    EXPECT_TRUE(wait_for([&] { return sessions->is_connected(*id); }));
    // server 收到 HELLO 后定向回 HelloAck（发送统计异步累加，轮询等待）。
    EXPECT_TRUE(wait_for([&] { return server.stats().tx_packets >= 1; }));

    client.stop();
    server.stop();
}

TEST(UdpProtocolTest, UnknownSessionHelloIsIgnored)
{
    asio::io_context io;
    auto sessions = std::make_shared<SessionManager>();
    const auto id = sessions->create_session(); // 存在的 session，但 HELLO 用别的 id
    ASSERT_TRUE(id.has_value());

    UdpServer server(io, sessions);
    ASSERT_TRUE(server.bind("127.0.0.1", 0));

    // 用裸 transport 发未创建 session 的 HELLO。
    UdpTransport sender(io);
    ASSERT_TRUE(sender.set_remote(server.local_endpoint()));

    IoThread thread(io);
    const auto hello = aqua::net::NetworkFrame::hello(0x12345678u).encode();
    for (int i = 0; i < 3; ++i) {
        sender.send(hello);
        std::this_thread::sleep_for(20ms);
    }

    EXPECT_FALSE(sessions->is_connected(0x12345678u));
    EXPECT_FALSE(sessions->is_connected(*id));
    // 未知 session 不回 Ack。
    EXPECT_EQ(server.stats().tx_packets, 0u);

    sender.stop();
    server.stop();
}

TEST(UdpProtocolTest, MalformedAndNonHelloDatagramsAreIgnored)
{
    asio::io_context io;
    auto sessions = std::make_shared<SessionManager>();
    const auto id = sessions->create_session();
    ASSERT_TRUE(id.has_value());

    UdpServer server(io, sessions);
    ASSERT_TRUE(server.bind("127.0.0.1", 0));

    UdpTransport sender(io);
    ASSERT_TRUE(sender.set_remote(server.local_endpoint()));

    IoThread thread(io);
    const std::vector<std::byte> short_packet(3); // 不足 Hello 长度
    sender.send(short_packet);
    const auto audio = aqua::net::NetworkFrame::audio(1, std::span<const std::byte> { }).encode();
    sender.send(audio);
    std::this_thread::sleep_for(50ms);

    EXPECT_FALSE(sessions->is_connected(*id));
    EXPECT_EQ(server.stats().tx_packets, 0u);

    sender.stop();
    server.stop();
}

TEST(UdpProtocolTest, ServerBroadcastsAudioToHelloHandshakeClient)
{
    asio::io_context io;
    auto sessions = std::make_shared<SessionManager>();
    const auto id = sessions->create_session();
    ASSERT_TRUE(id.has_value());

    UdpServer server(io, sessions);
    ASSERT_TRUE(server.bind("127.0.0.1", 0));
    ASSERT_TRUE(server.start());

    // client：收到 sequence + PCM span 后回填 promise（span 仅回调内有效，先拷贝）。
    UdpClient client(io);
    ASSERT_TRUE(client.set_remote("127.0.0.1", server.local_endpoint().port()));

    struct ReceivedFrame {
        std::uint64_t sequence = 0;
        std::uint32_t frame_count = 0;
        std::vector<std::byte> data;
    };
    auto received = std::make_shared<std::promise<ReceivedFrame>>();
    auto future = received->get_future();
    ASSERT_TRUE(client.start_receive(kFramesPerSlot * kFrameBytes, [received](std::uint64_t sequence, std::span<const std::byte> pcm) {
        received->set_value(ReceivedFrame { sequence, kFramesPerSlot,
            std::vector<std::byte>(pcm.begin(), pcm.end()) });
    }));

    IoThread thread(io);

    // 先握手（client 周期 HELLO → server establish），保证 NAT endpoint 就绪。
    client.start_hello(*id, 20ms);
    ASSERT_TRUE(wait_for([&] { return sessions->is_connected(*id); }));

    // server 广播一个 AudioFrame：client 侧应解出 sequence / F / payload。
    const auto payload = make_payload(0xAB);
    (void)server.broadcast(std::make_shared<const std::vector<std::byte>>(
        aqua::net::NetworkFrame::audio(7, payload).encode()));

    ASSERT_EQ(future.wait_for(2s), std::future_status::ready);
    const auto frame = future.get();
    EXPECT_EQ(frame.sequence, 7u);
    EXPECT_EQ(frame.frame_count, kFramesPerSlot);
    EXPECT_EQ(frame.data, payload);

    client.stop();
    server.stop();
}

TEST(UdpProtocolTest, RejectsAudioFromUnexpectedSender)
{
    asio::io_context io;
    auto sessions = std::make_shared<SessionManager>();
    UdpServer server(io, sessions);
    ASSERT_TRUE(server.bind("127.0.0.1", 0));
    ASSERT_TRUE(server.start());

    UdpClient client(io);
    ASSERT_TRUE(client.set_remote("127.0.0.1", server.local_endpoint().port()));
    std::atomic<unsigned> frame_calls { 0 };
    ASSERT_TRUE(client.start_receive(kFramesPerSlot * kFrameBytes,
        [&frame_calls](std::uint64_t, std::span<const std::byte>) {
            frame_calls.fetch_add(1, std::memory_order_relaxed);
        }));

    UdpTransport rogue(io);
    ASSERT_TRUE(rogue.open());

    IoThread thread(io);
    const auto payload = make_payload(0xCC);
    const auto audio = aqua::net::NetworkFrame::audio(123, payload).encode();
    rogue.send_to(client.local_endpoint(), audio);

    std::this_thread::sleep_for(100ms);
    EXPECT_EQ(frame_calls.load(std::memory_order_relaxed), 0u);

    rogue.stop();
    client.stop();
    server.stop();
}

TEST(UdpProtocolTest, EndpointDiscoveryLearnsAckSourceAndPinsAudio)
{
    // gRPC 通告的 endpoint 是 A，但 HELLO_ACK 实际来自 B（IPv6 隐私扩展/多地址服务器）。
    // client 应学习 B 作为音频 peer：来自 B 的音频接受，来自 A 的音频拒绝。
    asio::io_context io;

    asio::ip::udp::socket server_a(io, asio::ip::udp::v4());
    server_a.bind(asio::ip::udp::endpoint(asio::ip::address_v4::loopback(), 0));
    asio::ip::udp::socket server_b(io, asio::ip::udp::v4());
    server_b.bind(asio::ip::udp::endpoint(asio::ip::address_v4::loopback(), 0));

    UdpClient client(io);
    ASSERT_TRUE(client.set_remote("127.0.0.1", server_a.local_endpoint().port()));
    std::atomic<unsigned> frame_calls { 0 };
    ASSERT_TRUE(client.start_receive(kFramesPerSlot * kFrameBytes,
        [&frame_calls](std::uint64_t, std::span<const std::byte>) {
            frame_calls.fetch_add(1, std::memory_order_relaxed);
        }));

    IoThread thread(io);
    constexpr std::uint32_t kSession = 0x51525354u;
    client.start_hello(kSession, 20ms);

    const auto client_target = asio::ip::udp::endpoint(
        asio::ip::address_v4::loopback(), client.local_endpoint().port());

    // B 回正确 session 的 ACK → 学习 B。
    const auto ack = aqua::net::NetworkFrame::hello_ack(kSession).encode();
    server_b.send_to(asio::buffer(ack), client_target);
    ASSERT_TRUE(wait_for([&] { return client.hello_ack_count() >= 1; }));

    // learned_peer_endpoint() 应返回实际学到的 B（而非 gRPC 通告的 A）。
    const auto learned = client.learned_peer_endpoint();
    ASSERT_TRUE(learned.has_value());
    EXPECT_EQ(learned->port(), server_b.local_endpoint().port());

    // 来自 B 的音频接受。
    const auto payload = make_payload(0x5A);
    server_b.send_to(
        asio::buffer(aqua::net::NetworkFrame::audio(1, payload).encode()), client_target);
    ASSERT_TRUE(wait_for([&] { return frame_calls.load(std::memory_order_relaxed) >= 1; }));

    // 来自 A（gRPC 通告地址）的音频拒绝。
    server_a.send_to(
        asio::buffer(aqua::net::NetworkFrame::audio(2, payload).encode()), client_target);
    std::this_thread::sleep_for(100ms);
    EXPECT_EQ(frame_calls.load(std::memory_order_relaxed), 1u);

    client.stop();
}

TEST(UdpProtocolTest, EndpointRelocksOnLaterValidAck)
{
    // ACK 源迁移：先学 B，之后有效 ACK 来自 C（正确 session）→ 重锁 C；
    // 音频只认 C，旧 B 被拒绝。
    asio::io_context io;
    asio::ip::udp::socket remote(io, asio::ip::udp::v4());
    remote.bind(asio::ip::udp::endpoint(asio::ip::address_v4::loopback(), 0));
    asio::ip::udp::socket peer_b(io, asio::ip::udp::v4());
    peer_b.bind(asio::ip::udp::endpoint(asio::ip::address_v4::loopback(), 0));
    asio::ip::udp::socket peer_c(io, asio::ip::udp::v4());
    peer_c.bind(asio::ip::udp::endpoint(asio::ip::address_v4::loopback(), 0));

    UdpClient client(io);
    ASSERT_TRUE(client.set_remote("127.0.0.1", remote.local_endpoint().port()));
    std::atomic<unsigned> frame_calls { 0 };
    ASSERT_TRUE(client.start_receive(kFramesPerSlot * kFrameBytes,
        [&frame_calls](std::uint64_t, std::span<const std::byte>) {
            frame_calls.fetch_add(1, std::memory_order_relaxed);
        }));

    IoThread thread(io);
    constexpr std::uint32_t kSession = 0x61626364u;
    client.start_hello(kSession, 20ms);

    const auto client_target = asio::ip::udp::endpoint(
        asio::ip::address_v4::loopback(), client.local_endpoint().port());
    const auto payload = make_payload(0x5A);
    const auto ack = aqua::net::NetworkFrame::hello_ack(kSession).encode();

    // 学 B。
    peer_b.send_to(asio::buffer(ack), client_target);
    ASSERT_TRUE(wait_for([&] { return client.hello_ack_count() >= 1; }));
    peer_b.send_to(
        asio::buffer(aqua::net::NetworkFrame::audio(1, payload).encode()), client_target);
    ASSERT_TRUE(wait_for([&] { return frame_calls.load(std::memory_order_relaxed) >= 1; }));

    // 重锁 C。
    peer_c.send_to(asio::buffer(ack), client_target);
    ASSERT_TRUE(wait_for([&] { return client.hello_ack_count() >= 2; }));
    peer_c.send_to(
        asio::buffer(aqua::net::NetworkFrame::audio(2, payload).encode()), client_target);
    ASSERT_TRUE(wait_for([&] { return frame_calls.load(std::memory_order_relaxed) >= 2; }));

    // 旧 B 的音频拒绝。
    peer_b.send_to(
        asio::buffer(aqua::net::NetworkFrame::audio(3, payload).encode()), client_target);
    std::this_thread::sleep_for(100ms);
    EXPECT_EQ(frame_calls.load(std::memory_order_relaxed), 2u);

    client.stop();
}

TEST(UdpProtocolTest, WrongSessionAckDoesNotChangeLearnedEndpoint)
{
    // 已学 B；收到错误 session 的 ACK（来自 C）必须被拒，learned 仍是 B。
    asio::io_context io;
    asio::ip::udp::socket remote(io, asio::ip::udp::v4());
    remote.bind(asio::ip::udp::endpoint(asio::ip::address_v4::loopback(), 0));
    asio::ip::udp::socket peer_b(io, asio::ip::udp::v4());
    peer_b.bind(asio::ip::udp::endpoint(asio::ip::address_v4::loopback(), 0));
    asio::ip::udp::socket peer_c(io, asio::ip::udp::v4());
    peer_c.bind(asio::ip::udp::endpoint(asio::ip::address_v4::loopback(), 0));

    UdpClient client(io);
    ASSERT_TRUE(client.set_remote("127.0.0.1", remote.local_endpoint().port()));
    std::atomic<unsigned> frame_calls { 0 };
    ASSERT_TRUE(client.start_receive(kFramesPerSlot * kFrameBytes,
        [&frame_calls](std::uint64_t, std::span<const std::byte>) {
            frame_calls.fetch_add(1, std::memory_order_relaxed);
        }));

    IoThread thread(io);
    constexpr std::uint32_t kSession = 0x71727374u;
    client.start_hello(kSession, 20ms);

    const auto client_target = asio::ip::udp::endpoint(
        asio::ip::address_v4::loopback(), client.local_endpoint().port());
    const auto payload = make_payload(0x5A);

    // 学 B。
    const auto good_ack = aqua::net::NetworkFrame::hello_ack(kSession).encode();
    peer_b.send_to(asio::buffer(good_ack), client_target);
    ASSERT_TRUE(wait_for([&] { return client.hello_ack_count() >= 1; }));

    // 错误 session 的 ACK 来自 C → 拒绝，learned 不变。
    const auto bad_ack = aqua::net::NetworkFrame::hello_ack(kSession + 1).encode();
    peer_c.send_to(asio::buffer(bad_ack), client_target);
    std::this_thread::sleep_for(100ms);
    EXPECT_EQ(client.wrong_session_acks(), 1u);

    // B 的音频仍被接受，C 的音频被拒绝。
    peer_b.send_to(
        asio::buffer(aqua::net::NetworkFrame::audio(1, payload).encode()), client_target);
    ASSERT_TRUE(wait_for([&] { return frame_calls.load(std::memory_order_relaxed) >= 1; }));
    peer_c.send_to(
        asio::buffer(aqua::net::NetworkFrame::audio(2, payload).encode()), client_target);
    std::this_thread::sleep_for(100ms);
    EXPECT_EQ(frame_calls.load(std::memory_order_relaxed), 1u);

    client.stop();
}

TEST(UdpProtocolTest, ClientFiltersHelloAckFromFrameHandler)
{
    asio::io_context io;
    auto sessions = std::make_shared<SessionManager>();
    const auto id = sessions->create_session();
    ASSERT_TRUE(id.has_value());

    UdpServer server(io, sessions);
    ASSERT_TRUE(server.bind("127.0.0.1", 0));
    ASSERT_TRUE(server.start());

    UdpClient client(io);
    ASSERT_TRUE(client.set_remote("127.0.0.1", server.local_endpoint().port()));
    std::atomic<unsigned> frame_calls { 0 };
    ASSERT_TRUE(client.start_receive(kFramesPerSlot * kFrameBytes,
        [&frame_calls](std::uint64_t, std::span<const std::byte>) { frame_calls.fetch_add(1); }));
    client.start_hello(*id, 20ms);

    IoThread thread(io);
    // 握手期间 server 回的 HelloAck 不能进入帧回调。
    ASSERT_TRUE(wait_for([&] { return sessions->is_connected(*id); }));
    EXPECT_TRUE(wait_for([&] { return server.stats().tx_packets >= 1; }));
    std::this_thread::sleep_for(50ms);
    EXPECT_EQ(frame_calls.load(), 0u);

    client.stop();
    server.stop();
}

} // namespace
