// UDP 数据面协议层测试（UdpServer / UdpClient）：
// heartbeat 建连（establish + Ack 回复）与续命、未知 session / malformed 包忽略、
// 音频广播（UdpServer::broadcast → UdpClient datagram callback）、
// client 对 Heartbeat/HeartbeatAck 的内部过滤。
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

// RTP 测试音频包：seq/timestamp/SSRC 固定派生（timestamp 与 JB 无关，
// 本文件只验证传输/发现语义）。SSRC 默认统一；模拟“另一条流”时必须传
// 不同的 SSRC——同 SSRC = 同一条流（server 重定向会重锁），不同 SSRC
// 才是陌生流（来源不符即丢）。
constexpr std::uint32_t kTestSsrc = 0x11223344u;
constexpr std::uint32_t kOtherSsrc = 0x55667788u;

std::vector<std::byte> make_audio(std::uint16_t seq, const std::vector<std::byte>& payload,
    std::uint32_t ssrc = kTestSsrc)
{
    return aqua::net::NetworkFrame::audio(
        seq, static_cast<std::uint32_t>(seq) * 480u, ssrc, payload)
        .encode();
}

TEST(UdpProtocolTest, HeartbeatHandshakeEstablishesSession)
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
    client.start_heartbeat(*id, 20ms);

    IoThread thread(io);
    // heartbeat 首包 → server establish → session Connected。
    EXPECT_TRUE(wait_for([&] { return sessions->is_connected(*id); }));
    // server 建连后定向回 HeartbeatAck（发送统计异步累加，轮询等待）。
    EXPECT_TRUE(wait_for([&] { return server.stats().tx_packets >= 1; }));

    client.stop();
    server.stop();
}

TEST(UdpProtocolTest, UnknownSessionHeartbeatIsIgnored)
{
    asio::io_context io;
    auto sessions = std::make_shared<SessionManager>();
    const auto id = sessions->create_session(); // 存在的 session，但 heartbeat 用别的 id
    ASSERT_TRUE(id.has_value());

    UdpServer server(io, sessions);
    ASSERT_TRUE(server.bind("127.0.0.1", 0));

    // 用裸 transport 发未创建 session 的 heartbeat。
    UdpTransport sender(io);
    ASSERT_TRUE(sender.set_remote(server.local_endpoint()));

    IoThread thread(io);
    const auto hello = aqua::net::NetworkFrame::heartbeat(0x12345678u).encode();
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

TEST(UdpProtocolTest, MalformedAndNonHeartbeatDatagramsAreIgnored)
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
    const std::vector<std::byte> short_packet(3); // 不足 heartbeat 长度
    sender.send(short_packet);
    const auto audio = make_audio(1, std::vector<std::byte> { });
    sender.send(audio);
    std::this_thread::sleep_for(50ms);

    EXPECT_FALSE(sessions->is_connected(*id));
    EXPECT_EQ(server.stats().tx_packets, 0u);

    sender.stop();
    server.stop();
}

TEST(UdpProtocolTest, ServerBroadcastsAudioToHeartbeatHandshakeClient)
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

    // 先握手（client heartbeat 首包 → server establish），保证 NAT endpoint 就绪。
    client.start_heartbeat(*id, 20ms);
    ASSERT_TRUE(wait_for([&] { return sessions->is_connected(*id); }));

    // server 广播一个 AudioFrame：client 侧应解出 sequence / F / payload。
    const auto payload = make_payload(0xAB);
    (void)server.broadcast(std::make_shared<const std::vector<std::byte>>(
        make_audio(7, payload)));

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
    const auto audio = make_audio(123, payload);
    rogue.send_to(client.local_endpoint(), audio);

    std::this_thread::sleep_for(100ms);
    EXPECT_EQ(frame_calls.load(std::memory_order_relaxed), 0u);

    rogue.stop();
    client.stop();
    server.stop();
}

TEST(UdpProtocolTest, EndpointDiscoveryLearnsAckSourceAndPinsAudio)
{
    // gRPC 通告的 endpoint 是 A，但 HEARTBEAT_ACK 实际来自 B（IPv6 隐私扩展/多地址服务器）。
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
    client.start_heartbeat(kSession, 20ms);

    const auto client_target = asio::ip::udp::endpoint(
        asio::ip::address_v4::loopback(), client.local_endpoint().port());

    // B 回正确 session 的 ACK → 学习 B。
    const auto ack = aqua::net::NetworkFrame::heartbeat_ack(kSession).encode();
    server_b.send_to(asio::buffer(ack), client_target);
    ASSERT_TRUE(wait_for([&] { return client.hello_ack_count() >= 1; }));

    // learned_peer_endpoint() 应返回实际学到的 B（而非 gRPC 通告的 A）。
    const auto learned = client.learned_peer_endpoint();
    ASSERT_TRUE(learned.has_value());
    EXPECT_EQ(learned->port(), server_b.local_endpoint().port());

    // 来自 B 的音频接受。
    const auto payload = make_payload(0x5A);
    server_b.send_to(
        asio::buffer(make_audio(1, payload)), client_target);
    ASSERT_TRUE(wait_for([&] { return frame_calls.load(std::memory_order_relaxed) >= 1; }));

    // 来自 A（gRPC 通告地址）的音频拒绝：SSRC 与钉住流不同，是陌生流。
    server_a.send_to(
        asio::buffer(make_audio(2, payload, kOtherSsrc)), client_target);
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
    client.start_heartbeat(kSession, 20ms);

    const auto client_target = asio::ip::udp::endpoint(
        asio::ip::address_v4::loopback(), client.local_endpoint().port());
    const auto payload = make_payload(0x5A);
    const auto ack = aqua::net::NetworkFrame::heartbeat_ack(kSession).encode();

    // 学 B。
    peer_b.send_to(asio::buffer(ack), client_target);
    ASSERT_TRUE(wait_for([&] { return client.hello_ack_count() >= 1; }));
    peer_b.send_to(
        asio::buffer(make_audio(1, payload)), client_target);
    ASSERT_TRUE(wait_for([&] { return frame_calls.load(std::memory_order_relaxed) >= 1; }));

    // 重锁 C。
    peer_c.send_to(asio::buffer(ack), client_target);
    ASSERT_TRUE(wait_for([&] { return client.hello_ack_count() >= 2; }));
    peer_c.send_to(
        asio::buffer(make_audio(2, payload)), client_target);
    ASSERT_TRUE(wait_for([&] { return frame_calls.load(std::memory_order_relaxed) >= 2; }));

    // 旧 B 的音频拒绝：SSRC 与钉住流不同，是陌生流（同 SSRC 会被视为
    // 同一条流重定向而重锁——见 ServerSourceChangeRelearnedBySsrc）。
    peer_b.send_to(
        asio::buffer(make_audio(3, payload, kOtherSsrc)), client_target);
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
    client.start_heartbeat(kSession, 20ms);

    const auto client_target = asio::ip::udp::endpoint(
        asio::ip::address_v4::loopback(), client.local_endpoint().port());
    const auto payload = make_payload(0x5A);

    // 学 B。
    const auto good_ack = aqua::net::NetworkFrame::heartbeat_ack(kSession).encode();
    peer_b.send_to(asio::buffer(good_ack), client_target);
    ASSERT_TRUE(wait_for([&] { return client.hello_ack_count() >= 1; }));

    // 错误 session 的 ACK 来自 C → 拒绝，learned 不变。
    const auto bad_ack = aqua::net::NetworkFrame::heartbeat_ack(kSession + 1).encode();
    peer_c.send_to(asio::buffer(bad_ack), client_target);
    std::this_thread::sleep_for(100ms);
    EXPECT_EQ(client.wrong_session_acks(), 1u);

    // B 的音频仍被接受；C 的音频 SSRC 与钉住流不同，是陌生流，拒绝。
    peer_b.send_to(
        asio::buffer(make_audio(1, payload)), client_target);
    ASSERT_TRUE(wait_for([&] { return frame_calls.load(std::memory_order_relaxed) >= 1; }));
    peer_c.send_to(
        asio::buffer(make_audio(2, payload, kOtherSsrc)), client_target);
    std::this_thread::sleep_for(100ms);
    EXPECT_EQ(frame_calls.load(std::memory_order_relaxed), 1u);

    client.stop();
}

TEST(UdpProtocolTest, ClientFiltersHeartbeatAckFromFrameHandler)
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
    client.start_heartbeat(*id, 20ms);

    IoThread thread(io);
    // 握手期间 server 回的 HeartbeatAck 不能进入帧回调。
    ASSERT_TRUE(wait_for([&] { return sessions->is_connected(*id); }));
    EXPECT_TRUE(wait_for([&] { return server.stats().tx_packets >= 1; }));
    std::this_thread::sleep_for(50ms);
    EXPECT_EQ(frame_calls.load(), 0u);

    client.stop();
    server.stop();
}

TEST(UdpProtocolTest, HeartbeatEstablishesRefreshesAndRoams)
{
    asio::io_context io;
    auto sessions = std::make_shared<SessionManager>();
    const auto id = sessions->create_session();
    ASSERT_TRUE(id.has_value());

    UdpServer server(io, sessions);
    ASSERT_TRUE(server.bind("127.0.0.1", 0));
    ASSERT_TRUE(server.start());

    asio::ip::udp::socket sock_a(io, asio::ip::udp::v4());
    sock_a.bind(asio::ip::udp::endpoint(asio::ip::address_v4::loopback(), 0));
    const auto server_ep = asio::ip::udp::endpoint(
        asio::ip::address_v4::loopback(), server.local_endpoint().port());
    const auto hb = aqua::net::NetworkFrame::heartbeat(*id).encode();

    IoThread thread(io);

    // 首包即建连（Created→Connected），只回一次 ACK。
    // 先等 ACK 落定再取基线：Connected 对 session 表可见时，ACK 入队可能还在路上。
    sock_a.send_to(asio::buffer(hb), server_ep);
    ASSERT_TRUE(wait_for([&] { return sessions->is_connected(*id); }));
    ASSERT_TRUE(wait_for([&] { return server.heartbeat_ack_attempts() >= 1; }));
    EXPECT_EQ(server.sessions_established(), 1u);
    const auto acks_after_establish = server.heartbeat_ack_attempts();
    EXPECT_EQ(acks_after_establish, 1u);

    // 未知 session 的 heartbeat：计数但拒绝，不建连。
    const auto bad_hb = aqua::net::NetworkFrame::heartbeat(*id + 1).encode();
    sock_a.send_to(asio::buffer(bad_hb), server_ep);
    ASSERT_TRUE(wait_for([&] { return server.heartbeat_rejected() >= 1; }));

    // 同源续命 heartbeat：接受但不再回 ACK。
    sock_a.send_to(asio::buffer(hb), server_ep);
    ASSERT_TRUE(wait_for([&] { return server.heartbeat_received() >= 3; }));
    EXPECT_EQ(server.heartbeat_received(), 3u);
    EXPECT_EQ(server.heartbeat_ack_attempts(), acks_after_establish);

    // 漫游：另一源的 heartbeat 被接受并接管 endpoint，广播跟到新地址。
    asio::ip::udp::socket sock_b(io, asio::ip::udp::v4());
    sock_b.bind(asio::ip::udp::endpoint(asio::ip::address_v4::loopback(), 0));
    const auto hb_b = aqua::net::NetworkFrame::heartbeat(*id).encode();
    sock_b.send_to(asio::buffer(hb_b), server_ep);
    ASSERT_TRUE(wait_for([&] { return server.heartbeat_received() >= 4; }));
    EXPECT_EQ(server.heartbeat_received(), 4u);
    EXPECT_EQ(server.heartbeat_rejected(), 1u);

    auto received = std::make_shared<std::vector<std::byte>>(64);
    auto received_len = std::make_shared<std::size_t>(0);
    asio::ip::udp::endpoint sender_ep;
    sock_b.async_receive_from(asio::buffer(*received), sender_ep,
        [received, received_len](const asio::error_code& ec, std::size_t len) {
            if (!ec) {
                *received_len = len;
            }
        });
    const std::vector<std::byte> marker(1, std::byte { 0x7E });
    (void)server.broadcast(std::make_shared<const std::vector<std::byte>>(marker));
    ASSERT_TRUE(wait_for([&] { return *received_len > 0; }));
    EXPECT_EQ(*received_len, 1u);

    server.stop();
}

TEST(UdpProtocolTest, HeartbeatMaintainsSessionAfterHandshake)
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
    ASSERT_TRUE(client.start_receive(kFramesPerSlot * kFrameBytes,
        [](std::uint64_t, std::span<const std::byte>) { }));
    client.start_heartbeat(*id, 20ms);

    IoThread thread(io);
    ASSERT_TRUE(wait_for([&] { return sessions->is_connected(*id); }));

    // association 建立后 client 转 heartbeat 节奏（5s）：server 收到首个
    // heartbeat，且 heartbeat 计数不再增长（不再握手期高频发送）。
    // 先静置让在途包落定，再取基线，避免竞态误报。
    std::this_thread::sleep_for(100ms);
    const auto beats_before = server.heartbeat_received();
    ASSERT_TRUE(wait_for(
        [&] { return server.heartbeat_received() >= beats_before + 1; }, std::chrono::seconds(8)));
    EXPECT_EQ(server.heartbeat_rejected(), 0u);
    EXPECT_TRUE(sessions->is_connected(*id));
    std::this_thread::sleep_for(100ms);
    EXPECT_EQ(server.heartbeat_received(), beats_before + 1);

    client.stop();
    server.stop();
}

TEST(UdpProtocolTest, ServerSourceChangeRelearnedBySsrc)
{
    // server 上游重定向（IPv6 临时地址轮换/网卡/VPN）：音频源地址变了，
    // 但 SSRC 命中已钉住流 → 重锁 learned_endpoint 并继续接受；
    // SSRC 不对的陌生源仍然丢弃。
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
    constexpr std::uint32_t kSession = 0x32333435u;
    client.start_heartbeat(kSession, 20ms);

    const auto client_target = asio::ip::udp::endpoint(
        asio::ip::address_v4::loopback(), client.local_endpoint().port());
    const auto payload = make_payload(0x5A);
    const auto c_ep = asio::ip::udp::endpoint(
        asio::ip::address_v4::loopback(), peer_c.local_endpoint().port());

    // 建连学 B；B 的音频钉住 SSRC。
    peer_b.send_to(asio::buffer(aqua::net::NetworkFrame::heartbeat_ack(kSession).encode()),
        client_target);
    ASSERT_TRUE(wait_for([&] { return client.hello_ack_count() >= 1; }));
    peer_b.send_to(asio::buffer(make_audio(1, payload)), client_target);
    ASSERT_TRUE(wait_for([&] { return frame_calls.load(std::memory_order_relaxed) >= 1; }));

    // 同一 SSRC 换源到 C：重锁并接受。
    peer_c.send_to(asio::buffer(make_audio(2, payload)), client_target);
    ASSERT_TRUE(wait_for([&] { return frame_calls.load(std::memory_order_relaxed) >= 2; }));
    const auto learned = client.learned_peer_endpoint();
    ASSERT_TRUE(learned.has_value());
    EXPECT_EQ(learned->port(), c_ep.port());

    // SSRC 不对的陌生源（回 B）：丢弃，不回滚 learned。
    const auto rogue = aqua::net::NetworkFrame::audio(
        3, 3 * 480u, 0xDEADBEEFu, payload)
                           .encode();
    peer_b.send_to(asio::buffer(rogue), client_target);
    std::this_thread::sleep_for(100ms);
    EXPECT_EQ(frame_calls.load(std::memory_order_relaxed), 2u);
    EXPECT_EQ(client.learned_peer_endpoint()->port(), c_ep.port());

    client.stop();
}

} // namespace
