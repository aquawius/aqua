// 模块交互测试
//
// 验证多模块协同工作的真实场景：
//   - SessionManager + UdpTransport + packet：HELLO 握手 + 路由
//   - JitterBuffer + DiagnosticsManager：collect_and_log 读取 JB 状态
//   - SpscRingBuffer + JitterBuffer：JB pop -> RB write 背压联动
//   - SessionManager + packet：广播路由 + 过期清理
//   - UdpTransport + packet + JitterBuffer：真实 UDP 收发 -> JB
//   - SessionManager + 多 client 广播一致性
//   - HELLO 保活 + session 过期交互
//   - RuntimeConfig 端到端注入

#include "core/audio/ringbuffer/spsc_ringbuffer.h"
#include "core/diagnostics/diagnostics_manager.h"
#include "core/jitter_buffer/jitter_buffer.h"
#include "core/net/packet/packet.h"
#include "core/net/transport/udp_transport.h"
#include "core/public/audio_format.h"
#include "core/public/config.h"
#include "core/session/session_manager.h"

#include <gtest/gtest.h>

#include <asio.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

namespace {

aqua::AudioFormat make_test_format() {
    aqua::AudioFormat fmt;
    fmt.encoding = aqua::AudioEncoding::PcmF32LE;
    fmt.channels = 2;
    fmt.sample_rate = 48000;
    return fmt;
}

constexpr std::uint32_t FRAMES_PER_PACKET = 144;
constexpr std::size_t PAYLOAD_SIZE = 144 * 2 * 4;

std::vector<std::byte> make_payload(std::uint32_t sequence) {
    return std::vector<std::byte>(PAYLOAD_SIZE, static_cast<std::byte>((sequence & 0xFF) + 1));
}

} // namespace

// ==== 1. SessionManager + UdpTransport: HELLO 握手路由 ====
// server: 收到 HELLO -> establish_udp -> 回 HELLO_ACK
// client: 收到 HELLO_ACK -> 验证 session_id

TEST(ModuleIntegrationTest, HelloHandshakeRouting) {
    asio::io_context ioc;
    aqua::net::UdpTransport server_transport(ioc);
    aqua::net::UdpTransport client_transport(ioc);

    ASSERT_TRUE(server_transport.bind("127.0.0.1", 0));
    ASSERT_TRUE(client_transport.bind("127.0.0.1", 0));

    auto server_ep = server_transport.socket_local_endpoint();
    auto client_ep = client_transport.socket_local_endpoint();

    aqua::SessionManager sessions;
    auto session_id_opt = sessions.create_session();
    ASSERT_TRUE(session_id_opt.has_value());
    auto session_id = *session_id_opt;

    // server: 接收 HELLO -> establish -> 回 ACK
    server_transport.start_receive([&](const auto& sender, auto data) {
        auto type = aqua::net::peek_type(data);
        ASSERT_TRUE(type.has_value());
        ASSERT_EQ(*type, aqua::net::PacketType::Hello);

        auto hello = aqua::net::decode_hello(data);
        ASSERT_TRUE(hello.has_value());
        ASSERT_EQ(hello->session_id, session_id);

        // establish UDP（记录 NAT 后的 endpoint）
        EXPECT_TRUE(sessions.establish_udp(session_id, sender));
        EXPECT_TRUE(sessions.is_connected(session_id));

        // 回 HELLO_ACK
        std::array<std::byte, sizeof(aqua::net::HelloPacket)> ack_buf{};
        aqua::net::encode_hello_ack(session_id, ack_buf);
        server_transport.send(sender, ack_buf);
    });

    // client: 接收 HELLO_ACK
    std::atomic<bool> ack_received{false};
    std::atomic<std::uint32_t> ack_session_id{0};
    client_transport.start_receive([&](const auto& /*sender*/, auto data) {
        auto type = aqua::net::peek_type(data);
        if (type && *type == aqua::net::PacketType::HelloAck) {
            auto ack = aqua::net::decode_hello(data);
            if (ack) {
                ack_session_id.store(ack->session_id, std::memory_order_relaxed);
                ack_received.store(true, std::memory_order_relaxed);
            }
        }
    });

    std::thread ioc_thread([&] { ioc.run(); });

    // client 发送 HELLO
    std::array<std::byte, sizeof(aqua::net::HelloPacket)> hello_buf{};
    aqua::net::encode_hello(session_id, hello_buf);
    client_transport.send(server_ep, hello_buf);

    // 等待 ACK
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!ack_received.load(std::memory_order_relaxed)
           && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    EXPECT_TRUE(ack_received.load());
    EXPECT_EQ(ack_session_id.load(), session_id);

    // server 端 session endpoint 应记录为 client_ep
    auto ep = sessions.get_endpoint(session_id);
    ASSERT_TRUE(ep.has_value());
    EXPECT_EQ(ep->address(), client_ep.address());
    EXPECT_EQ(ep->port(), client_ep.port());

    server_transport.stop();
    client_transport.stop();
    ioc.stop();
    ioc_thread.join();
}

// ==== 2. SessionManager + packet: 多 client 广播一致性 ====
// server: for_each_connected 向所有 Connected session 发包
// 验证每个 client 收到相同数据

TEST(ModuleIntegrationTest, BroadcastToMultipleClientsConsistency) {
    asio::io_context ioc;
    aqua::net::UdpTransport server(ioc);

    ASSERT_TRUE(server.bind("127.0.0.1", 0));
    auto server_ep = server.socket_local_endpoint();

    constexpr int NUM_CLIENTS = 3;
    std::vector<std::unique_ptr<aqua::net::UdpTransport>> clients;
    std::vector<asio::ip::udp::endpoint> client_eps;
    std::vector<std::atomic<int>> recv_counts(NUM_CLIENTS);
    std::vector<std::vector<std::byte>> recv_data(NUM_CLIENTS);

    for (int i = 0; i < NUM_CLIENTS; ++i) {
        clients.push_back(std::make_unique<aqua::net::UdpTransport>(ioc));
        ASSERT_TRUE(clients.back()->bind("127.0.0.1", 0));
        client_eps.push_back(clients.back()->socket_local_endpoint());

        int idx = i;
        clients.back()->start_receive([&, idx](const auto& /*sender*/, auto data) {
            recv_counts[idx].fetch_add(1, std::memory_order_relaxed);
            recv_data[idx].assign(data.begin(), data.end());
        });
    }

    aqua::SessionManager sessions;
    for (int i = 0; i < NUM_CLIENTS; ++i) {
        auto id = sessions.create_session();
        ASSERT_TRUE(id.has_value());
        sessions.establish_udp(*id, client_eps[i]);
    }

    std::thread ioc_thread([&] { ioc.run(); });

    // server 广播一个 Audio 包
    auto payload = make_payload(42);
    std::vector<std::byte> packet(sizeof(aqua::net::AudioPacketHeader) + PAYLOAD_SIZE);
    auto n = aqua::net::encode_audio(0, 42, 42 * FRAMES_PER_PACKET, payload, packet);
    packet.resize(n);

    sessions.for_each_connected([&](auto /*id*/, const auto& ep) {
        server.send(ep, packet);
        return true;
    });

    // 等待所有 client 收到
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline) {
        bool all_received = true;
        for (int i = 0; i < NUM_CLIENTS; ++i) {
            if (recv_counts[i].load() == 0) { all_received = false; break; }
        }
        if (all_received) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // 每个 client 应收到 1 包，且数据完全一致
    for (int i = 0; i < NUM_CLIENTS; ++i) {
        EXPECT_EQ(recv_counts[i].load(), 1);
        EXPECT_EQ(recv_data[i], packet);
    }

    server.stop();
    for (auto& c : clients) c->stop();
    ioc.stop();
    ioc_thread.join();
}

// ==== 3. JitterBuffer + DiagnosticsManager: collect_and_log 读取 JB 状态 ====
// 验证 DiagManager 在 JB push/pop 过程中能正确读取 JB 状态

TEST(ModuleIntegrationTest, DiagnosticsReadsJitterBufferState) {
    std::size_t rb_fill = 0;
    aqua::diag::DiagnosticsManager dm(
        48000, 8, PAYLOAD_SIZE, [&rb_fill]() { return rb_fill; }, PAYLOAD_SIZE * 16);

    aqua::jitter::JitterBuffer jb(make_test_format(), FRAMES_PER_PACKET, 4, 16);

    // push 5 包
    for (std::uint32_t i = 0; i < 5; ++i) {
        jb.push(i, make_payload(i));
    }

    // 此时 JB 应有 5 包（buffer_fill_packets >= 1）
    EXPECT_GE(jb.buffer_fill_packets(), 1u);

    // collect_and_log 应正确反映
    dm.collect_and_log(jb);
    auto snap = dm.snapshot();

    EXPECT_EQ(snap.packets_received, 5u);
    EXPECT_GT(snap.jb_current_ms, 0.0);
    EXPECT_GT(snap.jb_capacity_ms, 0.0);  // 新增字段
    EXPECT_EQ(snap.jb_capacity_ms, jb.capacity_packets() * FRAMES_PER_PACKET * 1000.0 / 48000);
}

// ==== 4. SpscRingBuffer + JitterBuffer: pop -> write 背压联动 ====
// JB pop 后写入 RB，RB 满时应停止 pop（保留在 JB 中）

TEST(ModuleIntegrationTest, JitterBufferPopToRingBufferBackpressure) {
    aqua::jitter::JitterBuffer jb(make_test_format(), FRAMES_PER_PACKET, 2, 8);
    // 小容量 RB（对齐后 1024 字节，小于 PAYLOAD_SIZE=1152）
    aqua::audio::SpscRingBuffer rb(64);

    // push 5 包到 JB
    for (std::uint32_t i = 0; i < 5; ++i) {
        jb.push(i, make_payload(i));
    }

    // 模拟 client_main 的 pop -> write 循环
    std::vector<std::byte> pop_buf(PAYLOAD_SIZE);
    int popped = 0;
    int written_to_rb = 0;

    for (int i = 0; i < 30 && jb.buffer_fill_packets() > 0; ++i) {
        // 检查 RB 是否有空间
        if (rb.available_write() < PAYLOAD_SIZE) {
            break;  // RB 满，停止 pop（与 client_main 行为一致）
        }

        (void)jb.pop_next(pop_buf);
        ++popped;

        auto written = rb.write(pop_buf);
        written_to_rb += static_cast<int>(written);

        // RB 容量太小，第一次 write 后就应满
        if (written < PAYLOAD_SIZE) break;
    }

    // RB 容量对齐后 1024 字节 < PAYLOAD_SIZE 1152，available_write 初始 1024 < 1152，pop 前就 break
    // 这里验证背压机制：JB 不会在 RB 无空间时 pop
    EXPECT_EQ(popped, 0);  // RB 太小，一个都 pop 不了
    EXPECT_EQ(written_to_rb, 0);

    // JB 中包仍在
    EXPECT_EQ(jb.packets_received(), 5u);
    EXPECT_GE(jb.buffer_fill_packets(), 1u);
}

// ==== 5. SessionManager + 过期清理 + 广播联动 ====
// session 过期后不应再收到广播

TEST(ModuleIntegrationTest, ExpiredSessionExcludedFromBroadcast) {
    aqua::SessionManager sm;

    auto id1 = sm.create_session();
    auto id2 = sm.create_session();
    ASSERT_TRUE(id1.has_value());
    ASSERT_TRUE(id2.has_value());

    asio::ip::udp::endpoint ep1(asio::ip::make_address("127.0.0.1"), 10001);
    asio::ip::udp::endpoint ep2(asio::ip::make_address("127.0.0.1"), 10002);
    sm.establish_udp(*id1, ep1);
    sm.establish_udp(*id2, ep2);

    // 两个 session 都在广播列表
    int count = 0;
    sm.for_each_connected([&](auto, const auto&) { ++count; return true; });
    EXPECT_EQ(count, 2);

    // 等待 600ms 后 touch id2（保持 id2 活跃），再等 600ms 让 id1 过期但 id2 仍存活
    std::this_thread::sleep_for(std::chrono::milliseconds(600));
    sm.touch_session(*id2);
    std::this_thread::sleep_for(std::chrono::milliseconds(600));

    auto expired = sm.collect_expired_sessions(std::chrono::seconds(1));
    ASSERT_EQ(expired.size(), 1u);
    EXPECT_EQ(expired[0], *id1);
    sm.remove_session(*id1);

    // 广播列表只剩 id2
    count = 0;
    std::vector<aqua::SessionManager::session_id_t> visited;
    sm.for_each_connected([&](auto id, const auto&) {
        visited.push_back(id);
        ++count;
        return true;
    });
    EXPECT_EQ(count, 1);
    EXPECT_EQ(visited[0], *id2);
}

// ==== 6. UdpTransport + packet + JitterBuffer: 真实 UDP -> JB ====
// 真实 UDP loopback 收发 Audio 包，client 侧 decode -> JB.push

TEST(ModuleIntegrationTest, RealUdpToJitterBuffer) {
    asio::io_context ioc;
    aqua::net::UdpTransport sender(ioc);
    aqua::net::UdpTransport receiver(ioc);

    ASSERT_TRUE(sender.bind("127.0.0.1", 0));
    ASSERT_TRUE(receiver.bind("127.0.0.1", 0));
    auto recv_ep = receiver.socket_local_endpoint();

    aqua::jitter::JitterBuffer jb(make_test_format(), FRAMES_PER_PACKET, 4, 16);

    std::atomic<int> pushed{0};
    receiver.start_receive([&](const auto& /*sender*/, auto data) {
        auto decoded = aqua::net::decode_audio(data);
        if (decoded) {
            jb.push(decoded->header.sequence, decoded->payload);
            pushed.fetch_add(1, std::memory_order_relaxed);
        }
    });

    std::thread ioc_thread([&] { ioc.run(); });

    // 发送 10 个包
    constexpr int N = 10;
    for (int i = 0; i < N; ++i) {
        auto payload = make_payload(static_cast<std::uint32_t>(i));
        std::vector<std::byte> packet(sizeof(aqua::net::AudioPacketHeader) + PAYLOAD_SIZE);
        auto n = aqua::net::encode_audio(0, static_cast<std::uint32_t>(i),
                                         static_cast<std::uint32_t>(i) * FRAMES_PER_PACKET,
                                         payload, packet);
        packet.resize(n);
        sender.send(recv_ep, packet);
    }

    // 等待接收
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (pushed.load() < N && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    EXPECT_EQ(pushed.load(), N);
    EXPECT_EQ(jb.packets_received(), static_cast<std::uint64_t>(N));

    // pop 验证
    std::vector<std::byte> out(PAYLOAD_SIZE);
    int popped = 0;
    for (int i = 0; i < 30 && jb.buffer_fill_packets() > 0; ++i) {
        (void)jb.pop_next(out);
        ++popped;
    }
    EXPECT_EQ(popped, N);

    sender.stop();
    receiver.stop();
    ioc.stop();
    ioc_thread.join();
}

// ==== 7. HELLO 保活 + session 过期交互 ====
// 持续 HELLO 保活应阻止 session 过期

TEST(ModuleIntegrationTest, HelloKeepalivePreventsExpiration) {
    aqua::SessionManager sm;
    auto id = sm.create_session();
    ASSERT_TRUE(id.has_value());
    asio::ip::udp::endpoint ep(asio::ip::make_address("127.0.0.1"), 12345);
    sm.establish_udp(*id, ep);

    // 每 400ms touch 一次（模拟 HELLO 保活），持续 2s
    // 超时阈值设为 1s，若保活有效则不过期
    for (int i = 0; i < 5; ++i) {
        sm.touch_session(*id);
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
    }

    auto expired = sm.collect_expired_sessions(std::chrono::seconds(1));
    EXPECT_EQ(expired.size(), 0u);  // 保活有效，未过期
    EXPECT_TRUE(sm.is_connected(*id));

    // 停止保活，1.1s 后应过期
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    expired = sm.collect_expired_sessions(std::chrono::seconds(1));
    ASSERT_EQ(expired.size(), 1u);
    EXPECT_EQ(expired[0], *id);
}

// ==== 8. NAT remap: 同 session 重新 establish 更新 endpoint ====

TEST(ModuleIntegrationTest, NatRemapUpdatesEndpoint) {
    aqua::SessionManager sm;
    auto id = sm.create_session();
    ASSERT_TRUE(id.has_value());

    asio::ip::udp::endpoint ep1(asio::ip::make_address("127.0.0.1"), 50000);
    asio::ip::udp::endpoint ep2(asio::ip::make_address("127.0.0.1"), 60000);  // NAT remap 后新端口

    // 首次 establish
    sm.establish_udp(*id, ep1);
    auto current = sm.get_endpoint(*id);
    ASSERT_TRUE(current.has_value());
    EXPECT_EQ(current->port(), 50000);

    // NAT remap：同 session 不同端口再次 establish
    sm.establish_udp(*id, ep2);
    current = sm.get_endpoint(*id);
    ASSERT_TRUE(current.has_value());
    EXPECT_EQ(current->port(), 60000);  // 已更新

    // 广播应到新 endpoint
    asio::ip::udp::endpoint broadcast_ep;
    sm.for_each_connected([&](auto, const auto& ep) {
        broadcast_ep = ep;
        return true;
    });
    EXPECT_EQ(broadcast_ep.port(), 60000);
}

// ==== 9. RuntimeConfig 端到端注入：CLI 参数 -> RuntimeConfig -> JB/RB ====

TEST(ModuleIntegrationTest, RuntimeConfigEndToEndInjection) {
    // 模拟 client_main 的配置注入流程
    aqua::config::RuntimeConfig rt_cfg;

    // 模拟 CLI 参数覆盖
    rt_cfg.jitter_target_latency_ms = 15;  // --jitter-latency 15
    rt_cfg.playback_ringbuffer_size = 8192;  // --playback-buffer 8192
    rt_cfg.jitter_drift_late_threshold = 30;  // --drift-threshold 30

    // 构造 JB（与 client_main 相同的逻辑）
    const std::uint32_t frames_per_packet = aqua::config::AUDIO_FRAMES_PER_PACKET;
    std::size_t target_packets = (rt_cfg.jitter_target_latency_ms * 48000 / 1000) / frames_per_packet;
    ASSERT_EQ(target_packets, 5u);  // 15ms / 3ms = 5 包

    std::size_t capacity = 8;
    while (capacity < target_packets * 2) capacity <<= 1;
    ASSERT_EQ(capacity, 16u);

    aqua::jitter::JitterBuffer jb(make_test_format(), frames_per_packet, target_packets, capacity,
                                  aqua::config::JITTER_DRIFT_WINDOW_PACKETS,
                                  rt_cfg.jitter_drift_late_threshold);

    // 构造 RB
    aqua::audio::SpscRingBuffer rb(rt_cfg.playback_ringbuffer_size);
    // 8192 已是 1KiB 的倍数，对齐后仍为 8192
    EXPECT_EQ(rb.capacity(), 8192u);

    // 验证 JB 容量
    EXPECT_EQ(jb.capacity_packets(), 16u);

    // 端到端：push -> pop
    for (std::uint32_t i = 0; i < 10; ++i) {
        jb.push(i, make_payload(i));
    }
    std::vector<std::byte> out(PAYLOAD_SIZE);
    int popped = 0;
    for (int i = 0; i < 30 && jb.buffer_fill_packets() > 0; ++i) {
        (void)jb.pop_next(out);
        ++popped;
    }
    EXPECT_EQ(popped, 10);
}

// ==== 10. 多 session 并发广播 + JB 隔离 ====
// 每个 client 有独立 JB，验证互不干扰

TEST(ModuleIntegrationTest, MultipleClientsIndependentJitterBuffers) {
    constexpr int NUM_CLIENTS = 3;
    std::vector<std::unique_ptr<aqua::jitter::JitterBuffer>> jbs;
    for (int i = 0; i < NUM_CLIENTS; ++i) {
        jbs.push_back(std::make_unique<aqua::jitter::JitterBuffer>(
            make_test_format(), FRAMES_PER_PACKET, 4, 16));
    }

    // 模拟 server 广播：每个 client 收到相同的 10 包
    constexpr int N = 10;
    for (int seq = 0; seq < N; ++seq) {
        auto payload = make_payload(static_cast<std::uint32_t>(seq));
        for (auto& jb : jbs) {
            jb->push(static_cast<std::uint32_t>(seq), payload);
        }
    }

    // 每个 JB 独立 pop，结果应相同
    std::vector<std::vector<std::vector<std::byte>>> outputs(NUM_CLIENTS);
    for (int c = 0; c < NUM_CLIENTS; ++c) {
        std::vector<std::byte> out(PAYLOAD_SIZE);
        for (int i = 0; i < 30 && jbs[c]->buffer_fill_packets() > 0; ++i) {
            (void)jbs[c]->pop_next(out);
            outputs[c].push_back({out.begin(), out.end()});
        }
    }

    // 验证所有 client 输出一致
    for (int c = 1; c < NUM_CLIENTS; ++c) {
        EXPECT_EQ(outputs[c], outputs[0]);
    }

    // 每个 JB 统计应相同
    for (int c = 1; c < NUM_CLIENTS; ++c) {
        EXPECT_EQ(jbs[c]->packets_received(), jbs[0]->packets_received());
        EXPECT_EQ(jbs[c]->packets_lost(), jbs[0]->packets_lost());
    }
}

// ==== 11. packet decode_audio 零拷贝生命周期验证 ====
// DecodedAudio.payload 指向输入缓冲，验证输入缓冲修改后 payload 也变化

TEST(ModuleIntegrationTest, DecodeAudioZeroCopyLifetime) {
    std::vector<std::byte> packet(sizeof(aqua::net::AudioPacketHeader) + PAYLOAD_SIZE);
    auto payload = make_payload(42);
    auto n = aqua::net::encode_audio(0, 42, 42 * FRAMES_PER_PACKET, payload, packet);
    packet.resize(n);

    auto decoded = aqua::net::decode_audio(packet);
    ASSERT_TRUE(decoded.has_value());

    // payload 指向 packet 内部
    EXPECT_GE(decoded->payload.data(), packet.data());
    EXPECT_LT(decoded->payload.data(), packet.data() + packet.size());

    // 修改原缓冲，payload 应同步变化（零拷贝语义）
    auto first_byte = decoded->payload[0];
    packet[sizeof(aqua::net::AudioPacketHeader)] = std::byte{0xFF};
    EXPECT_EQ(decoded->payload[0], std::byte{0xFF});
    EXPECT_NE(decoded->payload[0], first_byte);
}

// ==== 12. DiagnosticsManager + RingBuffer 容量字段验证 ====
// 新增的 jb_capacity_ms / rb_capacity_ms 字段

TEST(ModuleIntegrationTest, DiagnosticsCapacityFieldsPopulated) {
    std::size_t rb_fill = 0;
    constexpr std::size_t RB_CAP = 8192;
    aqua::diag::DiagnosticsManager dm(
        48000, 8, PAYLOAD_SIZE, [&rb_fill]() { return rb_fill; }, RB_CAP);

    aqua::jitter::JitterBuffer jb(make_test_format(), FRAMES_PER_PACKET, 4, 16);

    // push 一些包
    for (int i = 0; i < 5; ++i) {
        jb.push(static_cast<std::uint32_t>(i),
                make_payload(static_cast<std::uint32_t>(i)));
    }
    rb_fill = 1024;

    dm.collect_and_log(jb);
    auto snap = dm.snapshot();

    // JB capacity: 16 packets × 3ms = 48ms
    EXPECT_NEAR(snap.jb_capacity_ms, 48.0, 0.5);
    // RB capacity: 8192 bytes / 8 (frame_bytes) × 1000 / 48000 = 21.33ms
    EXPECT_NEAR(snap.rb_capacity_ms, 21.33, 0.5);

    // 当前水位应 <= 容量
    EXPECT_LE(snap.jb_current_ms, snap.jb_capacity_ms);
    EXPECT_LE(snap.rb_current_ms, snap.rb_capacity_ms);
}
