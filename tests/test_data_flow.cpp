// 端到端数据流测试
//
// 模拟完整的 server -> client 音频管线，验证数据完整性：
//   1. 内存模拟: capture callback -> ringbuffer -> packetizer encode ->
//      for_each_connected broadcast -> client decode -> ringbuffer -> playback fill
//   2. 真实 UDP loopback: 两个 UdpTransport 实例通过 127.0.0.1 收发，
//      验证实际网络传输下的数据完整性
//
// 覆盖场景：
//   - 多包连续传输的字节级校验
//   - sequence / sample_position 递增正确性
//   - 多 client 广播一致性
//   - RingBuffer 背压（溢出丢包）
//   - 真实 UDP 丢包容忍
//   - 大 payload 传输

#include <gtest/gtest.h>

#include "core/audio/ringbuffer/spsc_ringbuffer.h"
#include "core/net/packet/packet.h"
#include "core/net/transport/udp_transport.h"
#include "core/public/audio_format.h"
#include "core/public/config.h"
#include "core/session/session_manager.h"

#include <asio.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>
#include <vector>

using aqua::audio::SpscRingBuffer;
using aqua::net::AudioPacketHeader;
using aqua::net::PacketType;
using aqua::SessionManager;

namespace {

// 模拟 server packetizer: 从 ringbuffer 读取一帧 PCM, 编码为 Audio 包
// 返回写入 out 的字节数 (header + payload), 0 表示 ringbuffer 无足够数据
std::size_t server_packetize(SpscRingBuffer& capture_rb,
                              std::uint32_t session_id,
                              std::uint32_t sequence,
                              std::uint32_t sample_position,
                              std::size_t payload_size,
                              std::span<std::byte> out,
                              std::span<std::byte> pcm_scratch)
{
    if (pcm_scratch.size() < payload_size) return 0;
    if (out.size() < sizeof(AudioPacketHeader) + payload_size) return 0;

    std::size_t got = 0;
    while (got < payload_size) {
        std::size_t r = capture_rb.read(std::span<std::byte>{
            pcm_scratch.data() + got, payload_size - got});
        if (r == 0) break;  // 无数据
        got += r;
    }
    if (got < payload_size) return 0;

    return aqua::net::encode_audio(session_id, sequence, sample_position,
                                    std::span<const std::byte>{pcm_scratch.data(), got},
                                    out);
}

// 模拟 client 接收: 解码 Audio 包, payload 写入 playback ringbuffer
// 返回写入的字节数, -1 表示解码失败
int client_receive(std::span<const std::byte> packet, SpscRingBuffer& playback_rb)
{
    auto decoded = aqua::net::decode_audio(packet);
    if (!decoded) return -1;
    return static_cast<int>(playback_rb.write(decoded->payload));
}

// 生成可识别的 PCM pattern: 每个采样 = (sequence * 1000 + sample_offset_in_packet)
void fill_pattern_pcm(std::span<std::byte> pcm, std::uint32_t sequence,
                      std::uint32_t frame_bytes)
{
    const std::size_t frame_count = pcm.size() / frame_bytes;
    for (std::size_t i = 0; i < frame_count; ++i) {
        std::uint32_t val = sequence * 1000 + static_cast<std::uint32_t>(i);
        // 简单填充: 每个字节用 (val + byte_index_in_frame) & 0xFF
        for (std::size_t b = 0; b < frame_bytes; ++b) {
            pcm[i * frame_bytes + b] = std::byte{
                static_cast<uint8_t>((val + b) & 0xFF)};
        }
    }
}

// 验证 PCM pattern
bool verify_pattern_pcm(std::span<const std::byte> pcm, std::uint32_t sequence,
                         std::uint32_t frame_bytes)
{
    const std::size_t frame_count = pcm.size() / frame_bytes;
    for (std::size_t i = 0; i < frame_count; ++i) {
        std::uint32_t val = sequence * 1000 + static_cast<std::uint32_t>(i);
        for (std::size_t b = 0; b < frame_bytes; ++b) {
            std::byte expected{
                static_cast<uint8_t>((val + b) & 0xFF)};
            if (pcm[i * frame_bytes + b] != expected) return false;
        }
    }
    return true;
}

} // namespace

// ==== 内存模拟: 完整管线单包传输 ====

TEST(DataFlowTest, InMemorySinglePacketEndToEnd)
{
    // 模拟 48kHz / S16LE / 立体声, 10ms 一包
    aqua::AudioFormat fmt{aqua::AudioEncoding::PcmS16LE, 2, 48000};
    const std::uint32_t frames_per_packet = fmt.sample_rate * 10 / 1000;  // 480
    const std::size_t payload_size = frames_per_packet * fmt.frame_bytes();  // 1920

    SpscRingBuffer capture_rb(64 * 1024);
    SpscRingBuffer playback_rb(64 * 1024);

    // 1. 模拟 capture: 写入一包 PCM
    std::vector<std::byte> pcm(payload_size);
    fill_pattern_pcm(pcm, 42, fmt.frame_bytes());
    ASSERT_EQ(capture_rb.write(pcm), payload_size);

    // 2. server packetize
    std::vector<std::byte> packet(sizeof(AudioPacketHeader) + payload_size);
    std::vector<std::byte> scratch(payload_size);
    auto written = server_packetize(capture_rb, 0x12345678, 42, 42 * 480,
                                     payload_size, packet, scratch);
    ASSERT_EQ(written, sizeof(AudioPacketHeader) + payload_size);

    // 3. client receive
    int recv = client_receive(std::span<const std::byte>{packet.data(), written}, playback_rb);
    ASSERT_EQ(recv, static_cast<int>(payload_size));

    // 4. playback 读取并校验
    std::vector<std::byte> out(payload_size);
    ASSERT_EQ(playback_rb.read(out), payload_size);
    EXPECT_TRUE(verify_pattern_pcm(out, 42, fmt.frame_bytes()));
}

// ==== 内存模拟: 多包连续传输 + sequence 递增 ====

TEST(DataFlowTest, InMemoryMultiplePacketsSequenceIncrement)
{
    aqua::AudioFormat fmt{aqua::AudioEncoding::PcmS16LE, 2, 48000};
    const std::uint32_t frames_per_packet = 480;
    const std::size_t payload_size = frames_per_packet * fmt.frame_bytes();

    SpscRingBuffer capture_rb(256 * 1024);
    SpscRingBuffer playback_rb(256 * 1024);

    std::vector<std::byte> packet(sizeof(AudioPacketHeader) + payload_size);
    std::vector<std::byte> scratch(payload_size);

    constexpr int NUM_PACKETS = 20;

    // 发送 NUM_PACKETS 个包
    for (std::uint32_t seq = 0; seq < NUM_PACKETS; ++seq) {
        // capture 写入
        std::vector<std::byte> pcm(payload_size);
        fill_pattern_pcm(pcm, seq, fmt.frame_bytes());
        ASSERT_EQ(capture_rb.write(pcm), payload_size);

        // packetize
        auto written = server_packetize(capture_rb, 0, seq, seq * frames_per_packet,
                                         payload_size, packet, scratch);
        ASSERT_GT(written, 0u);

        // client receive
        ASSERT_GT(client_receive(std::span<const std::byte>{packet.data(), written}, playback_rb), 0);
    }

    // 接收并校验所有包
    std::vector<std::byte> out(payload_size);
    for (std::uint32_t seq = 0; seq < NUM_PACKETS; ++seq) {
        ASSERT_EQ(playback_rb.read(out), payload_size)
            << "Failed at seq=" << seq;
        EXPECT_TRUE(verify_pattern_pcm(out, seq, fmt.frame_bytes()))
            << "Pattern mismatch at seq=" << seq;
    }
}

// ==== 内存模拟: 多 client 广播一致性 ====

TEST(DataFlowTest, InMemoryBroadcastToMultipleClients)
{
    aqua::AudioFormat fmt{aqua::AudioEncoding::PcmF32LE, 2, 48000};
    const std::uint32_t frames_per_packet = 480;
    const std::size_t payload_size = frames_per_packet * fmt.frame_bytes();

    SpscRingBuffer capture_rb(64 * 1024);
    constexpr int NUM_CLIENTS = 5;
    std::vector<std::unique_ptr<SpscRingBuffer>> playback_rbs;
    for (int i = 0; i < NUM_CLIENTS; ++i) {
        playback_rbs.push_back(std::make_unique<SpscRingBuffer>(64 * 1024));
    }

    // capture 写入
    std::vector<std::byte> pcm(payload_size);
    fill_pattern_pcm(pcm, 7, fmt.frame_bytes());
    capture_rb.write(pcm);

    // packetize
    std::vector<std::byte> packet(sizeof(AudioPacketHeader) + payload_size);
    std::vector<std::byte> scratch(payload_size);
    auto written = server_packetize(capture_rb, 0, 7, 0, payload_size, packet, scratch);
    ASSERT_GT(written, 0u);

    // 广播到所有 client
    for (auto& rb : playback_rbs) {
        client_receive(std::span<const std::byte>{packet.data(), written}, *rb);
    }

    // 所有 client 收到相同数据
    std::vector<std::byte> out(payload_size);
    for (int i = 0; i < NUM_CLIENTS; ++i) {
        ASSERT_EQ(playback_rbs[i]->read(out), payload_size);
        EXPECT_TRUE(verify_pattern_pcm(out, 7, fmt.frame_bytes()))
            << "Client " << i << " received wrong data";
    }
}

// ==== RingBuffer 背压: 溢出时返回部分写入 ====

TEST(DataFlowTest, RingBufferOverflowReturnsPartialWrite)
{
    SpscRingBuffer rb(128);  // 容量 128 字节

    // 第一次写满
    std::vector<std::byte> data(128, std::byte{0xAA});
    EXPECT_EQ(rb.write(data), 128u);

    // 第二次写应该返回 0 (缓冲已满)
    EXPECT_EQ(rb.write(data), 0u);
    EXPECT_EQ(rb.available_write(), 0u);

    // 读取一部分后, 可以再写
    std::vector<std::byte> out(64);
    EXPECT_EQ(rb.read(out), 64u);
    EXPECT_EQ(rb.write(std::span<const std::byte>{data.data(), 32}), 32u);
}

// ==== RingBuffer 背压: 模拟 capture 快于 packetizer 的丢包场景 ====

TEST(DataFlowTest, CaptureFasterThanPacketizerDropsData)
{
    SpscRingBuffer rb(128);  // 小容量, 容易溢出

    std::vector<std::byte> chunk(64, std::byte{0x11});
    std::size_t total_written = 0;
    std::size_t total_attempted = 0;

    // 连续写 10 个 chunk, 每次尝试 64 字节
    for (int i = 0; i < 10; ++i) {
        total_attempted += 64;
        total_written += rb.write(chunk);
    }

    // 容量 128, 应该只写入 2 个完整 chunk = 128 字节
    EXPECT_EQ(total_written, 128u);
    EXPECT_LT(total_written, total_attempted);  // 确实有丢包

    // 读出的数据应该都是 0x11 (写入的 pattern)
    std::vector<std::byte> out(128);
    EXPECT_EQ(rb.read(out), 128u);
    for (auto b : out) EXPECT_EQ(b, std::byte{0x11});
}

// ==== 真实 UDP loopback: 单包传输 ====

TEST(DataFlowTest, UdpLoopbackSinglePacket)
{
    asio::io_context ioc;

    aqua::net::UdpTransport server(ioc);
    aqua::net::UdpTransport client(ioc);

    ASSERT_TRUE(server.bind("127.0.0.1", 0));
    ASSERT_TRUE(client.bind("127.0.0.1", 0));

    auto server_ep = server.socket_local_endpoint();
    auto client_ep = client.socket_local_endpoint();

    std::atomic<bool> received{false};
    std::vector<std::byte> recv_data;

    client.start_receive([&](const auto& /*sender*/, std::span<const std::byte> data) {
        recv_data.assign(data.begin(), data.end());
        received = true;
    });

    std::thread ioc_thread([&] { ioc.run(); });

    // server -> client 发送一个 Audio 包
    std::vector<std::byte> pcm(480, std::byte{0x42});
    std::vector<std::byte> packet(sizeof(AudioPacketHeader) + pcm.size());
    auto written = aqua::net::encode_audio(0xCAFEBABE, 99, 9999, pcm, packet);
    ASSERT_GT(written, 0u);

    server.send(client_ep, std::span<const std::byte>{packet.data(), written});

    for (int i = 0; i < 100 && !received; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ASSERT_TRUE(received);

    // 校验接收到的包
    auto decoded = aqua::net::decode_audio(std::span<const std::byte>{recv_data.data(), recv_data.size()});
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->header.session_id, 0xCAFEBABEu);
    EXPECT_EQ(decoded->header.sequence, 99u);
    EXPECT_EQ(decoded->header.sample_position, 9999u);
    EXPECT_EQ(decoded->payload.size(), 480u);
    EXPECT_EQ(decoded->payload[0], std::byte{0x42});

    ioc.stop();
    ioc_thread.join();
}

// ==== 真实 UDP loopback: 多包传输 + ringbuffer ====

TEST(DataFlowTest, UdpLoopbackMultiplePacketsIntoRingBuffer)
{
    asio::io_context ioc;
    aqua::net::UdpTransport server(ioc);
    aqua::net::UdpTransport client(ioc);

    ASSERT_TRUE(server.bind("127.0.0.1", 0));
    ASSERT_TRUE(client.bind("127.0.0.1", 0));
    auto client_ep = client.socket_local_endpoint();

    SpscRingBuffer playback_rb(64 * 1024);
    std::atomic<int> packets_received{0};

    client.start_receive([&](const auto&, std::span<const std::byte> data) {
        auto decoded = aqua::net::decode_audio(data);
        if (decoded) {
            playback_rb.write(decoded->payload);
            packets_received.fetch_add(1, std::memory_order_relaxed);
        }
    });

    std::thread ioc_thread([&] { ioc.run(); });

    // 发送 10 个包, 每包不同 pattern
    constexpr int NUM_PACKETS = 10;
    std::vector<std::byte> pcm(960, std::byte{0});  // 480 frames * 2 bytes * 1ch (简化)
    std::vector<std::byte> packet(sizeof(AudioPacketHeader) + pcm.size());

    for (int seq = 0; seq < NUM_PACKETS; ++seq) {
        std::fill(pcm.begin(), pcm.end(), std::byte{static_cast<uint8_t>(seq)});
        auto written = aqua::net::encode_audio(0, seq, seq * 480, pcm, packet);
        server.send(client_ep, std::span<const std::byte>{packet.data(), written});
    }

    // 等待接收 (UDP 可能丢包, 至少收到一部分)
    for (int i = 0; i < 200 && packets_received.load() < NUM_PACKETS; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_GE(packets_received.load(), 1);

    // 校验 ringbuffer 中有数据
    EXPECT_GT(playback_rb.available_read(), 0u);

    ioc.stop();
    ioc_thread.join();
}

// ==== 真实 UDP loopback: HELLO 握手 + ACK ====

TEST(DataFlowTest, UdpLoopbackHelloHandshake)
{
    asio::io_context ioc;
    aqua::net::UdpTransport server(ioc);
    aqua::net::UdpTransport client(ioc);

    ASSERT_TRUE(server.bind("127.0.0.1", 0));
    ASSERT_TRUE(client.bind("127.0.0.1", 0));

    auto server_ep = server.socket_local_endpoint();
    auto client_ep = client.socket_local_endpoint();

    SessionManager sm;
    auto sid = sm.create_session();
    ASSERT_TRUE(sid.has_value());

    std::atomic<bool> ack_received{false};

    // server: 收到 HELLO -> establish + 回 ACK
    server.start_receive([&](const auto& sender, std::span<const std::byte> data) {
        auto type = aqua::net::peek_type(data);
        if (type && *type == PacketType::Hello) {
            auto hello = aqua::net::decode_hello(data);
            if (hello) {
                sm.establish_udp(hello->session_id, sender);
                std::array<std::byte, sizeof(aqua::net::HelloPacket)> ack{};
                aqua::net::encode_hello_ack(hello->session_id, ack);
                server.send(sender, std::span<const std::byte>{ack.data(), ack.size()});
            }
        }
    });

    // client: 收到 ACK
    client.start_receive([&](const auto&, std::span<const std::byte> data) {
        auto type = aqua::net::peek_type(data);
        if (type && *type == PacketType::HelloAck) {
            auto ack = aqua::net::decode_hello(data);
            if (ack && ack->session_id == *sid) {
                ack_received = true;
            }
        }
    });

    std::thread ioc_thread([&] { ioc.run(); });

    // client 发 HELLO
    std::array<std::byte, sizeof(aqua::net::HelloPacket)> hello{};
    aqua::net::encode_hello(*sid, hello);
    client.send(server_ep, std::span<const std::byte>{hello.data(), hello.size()});

    for (int i = 0; i < 100 && !ack_received; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    EXPECT_TRUE(ack_received);
    EXPECT_TRUE(sm.is_connected(*sid));
    EXPECT_EQ(sm.get_endpoint(*sid).value(), client_ep);

    ioc.stop();
    ioc_thread.join();
}

// ==== 真实 UDP loopback: 大 payload 传输 ====

TEST(DataFlowTest, UdpLoopbackLargePayload)
{
    asio::io_context ioc;
    aqua::net::UdpTransport server(ioc);
    aqua::net::UdpTransport client(ioc);

    ASSERT_TRUE(server.bind("127.0.0.1", 0));
    ASSERT_TRUE(client.bind("127.0.0.1", 0));
    auto client_ep = client.socket_local_endpoint();

    // 接近 UDP datagram 上限 (65535 - header)
    constexpr std::size_t LARGE_PAYLOAD = 60000;
    std::atomic<bool> received{false};
    std::size_t recv_payload_size = 0;

    client.start_receive([&](const auto&, std::span<const std::byte> data) {
        auto decoded = aqua::net::decode_audio(data);
        if (decoded) {
            recv_payload_size = decoded->payload.size();
            received = true;
        }
    });

    std::thread ioc_thread([&] { ioc.run(); });

    std::vector<std::byte> large_pcm(LARGE_PAYLOAD, std::byte{0x77});
    std::vector<std::byte> packet(sizeof(AudioPacketHeader) + LARGE_PAYLOAD);
    auto written = aqua::net::encode_audio(1, 1, 1, large_pcm, packet);
    ASSERT_GT(written, 0u);

    server.send(client_ep, std::span<const std::byte>{packet.data(), written});

    for (int i = 0; i < 100 && !received; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // 大包可能被 IP 分片, 在某些环境下会丢失; 收到则校验大小
    if (received) {
        EXPECT_EQ(recv_payload_size, LARGE_PAYLOAD);
    }

    ioc.stop();
    ioc_thread.join();
}

// ==== SessionManager + 真实 UDP: 完整握手 + 数据广播 ====

TEST(DataFlowTest, FullHandshakeAndBroadcastFlow)
{
    asio::io_context ioc;
    aqua::net::UdpTransport server_transport(ioc);
    aqua::net::UdpTransport client_transport(ioc);

    ASSERT_TRUE(server_transport.bind("127.0.0.1", 0));
    ASSERT_TRUE(client_transport.bind("127.0.0.1", 0));

    auto server_ep = server_transport.socket_local_endpoint();

    SessionManager sm;
    auto sid = sm.create_session();
    ASSERT_TRUE(sid.has_value());

    std::atomic<bool> acked{false};
    std::atomic<int> audio_recv{0};

    // server: 处理 HELLO + (后续) 不处理 Audio (单向)
    server_transport.start_receive([&](const auto& sender, std::span<const std::byte> data) {
        auto type = aqua::net::peek_type(data);
        if (type && *type == PacketType::Hello) {
            auto hello = aqua::net::decode_hello(data);
            if (hello && sm.establish_udp(hello->session_id, sender)) {
                std::array<std::byte, sizeof(aqua::net::HelloPacket)> ack{};
                aqua::net::encode_hello_ack(hello->session_id, ack);
                server_transport.send(sender, std::span<const std::byte>{ack.data(), ack.size()});
            }
        }
    });

    // client: 处理 ACK + Audio
    client_transport.start_receive([&](const auto&, std::span<const std::byte> data) {
        auto type = aqua::net::peek_type(data);
        if (!type) return;
        if (*type == PacketType::HelloAck) {
            auto ack = aqua::net::decode_hello(data);
            if (ack && ack->session_id == *sid) acked = true;
        } else if (*type == PacketType::Audio) {
            audio_recv.fetch_add(1, std::memory_order_relaxed);
        }
    });

    std::thread ioc_thread([&] { ioc.run(); });

    // 1. 握手
    std::array<std::byte, sizeof(aqua::net::HelloPacket)> hello{};
    aqua::net::encode_hello(*sid, hello);
    client_transport.send(server_ep, std::span<const std::byte>{hello.data(), hello.size()});

    for (int i = 0; i < 100 && !acked; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ASSERT_TRUE(acked);
    ASSERT_TRUE(sm.is_connected(*sid));

    // 2. server 广播 5 个 Audio 包
    std::vector<std::byte> pcm(480, std::byte{0x55});
    std::vector<std::byte> packet(sizeof(AudioPacketHeader) + pcm.size());
    for (int seq = 0; seq < 5; ++seq) {
        auto written = aqua::net::encode_audio(0, seq, seq * 480, pcm, packet);
        sm.for_each_connected([&](auto, const auto& ep) {
            server_transport.send(ep, std::span<const std::byte>{packet.data(), written});
            return true;
        });
    }

    // 等待接收
    for (int i = 0; i < 100 && audio_recv.load() < 5; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_GE(audio_recv.load(), 1);

    ioc.stop();
    ioc_thread.join();
}

// ==== 模拟 client 数据接收超时检测逻辑 ====
// 验证: 超过阈值未收到 Audio 包时, "client" 应检测到超时

TEST(DataFlowTest, ClientAudioTimeoutDetectionLogic)
{
    // 模拟 client_main 中的超时检测逻辑
    constexpr auto TIMEOUT = std::chrono::seconds(2);
    auto last_recv = std::chrono::steady_clock::now();

    // 立即检查: 不应超时
    auto now = std::chrono::steady_clock::now();
    EXPECT_LE(now - last_recv, TIMEOUT);

    // 等待超过阈值
    std::this_thread::sleep_for(TIMEOUT + std::chrono::milliseconds(100));
    now = std::chrono::steady_clock::now();
    EXPECT_GT(now - last_recv, TIMEOUT);

    // 模拟收到数据: 更新时间戳
    last_recv = std::chrono::steady_clock::now();
    now = std::chrono::steady_clock::now();
    EXPECT_LE(now - last_recv, TIMEOUT);
}
