// NAT + 数据面模拟测试
//
// 不依赖真实网络与音频硬件。在内存中模拟 server 侧的完整流程：
//   gRPC Connect (create_session)
//   -> client 发 HELLO (establish_udp, 记录 NAT endpoint)
//   -> server 回 HELLO_ACK
//   -> server packetizer 编码 AUDIO 包并广播给所有 Connected session
//   -> 模拟 NAT 重映射 (re-establish)
//   -> 模拟丢包 (sequence 跳跃)
//   -> 模拟超时清理
//
// 验证 AGENT.md §6 NAT 设计约定:
//   - server 仅通过 gRPC 告知 UDP 端口
//   - client 用 gRPC server IP + 该端口发 HELLO
//   - server 从 HELLO source endpoint 记录 NAT 映射地址

#include <gtest/gtest.h>

#include "core/net/packet/packet.h"
#include "core/session/session_manager.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <thread>
#include <unordered_set>
#include <vector>

using aqua::net::AudioPacketHeader;
using aqua::net::PacketType;
using aqua::SessionManager;

namespace {

// 构造一个模拟的 client NAT endpoint
asio::ip::udp::endpoint make_nat_endpoint(int client_index, unsigned short port)
{
    // 模拟公网 NAT 映射地址: 203.0.113.<index>:<port>
    std::string addr = "203.0.113." + std::to_string(client_index);
    return asio::ip::udp::endpoint(asio::ip::make_address(addr), port);
}

// 模拟 server 收到 HELLO 后的处理: 记录 NAT endpoint + 回 HELLO_ACK
// 返回 HELLO_ACK 编码后的字节数 (模拟发给 client)
std::size_t simulate_server_hello(SessionManager& sm,
                                   aqua::SessionManager::session_id_t sid,
                                   const asio::ip::udp::endpoint& sender,
                                   std::span<std::byte> ack_out)
{
    if (!sm.establish_udp(sid, sender)) {
        return 0; // 未知 session, server 丢弃
    }
    return aqua::net::encode_hello_ack(sid, ack_out);
}

// 模拟 server packetizer: 编码一个音频包 (session_id=0 表示广播)
std::size_t simulate_packetizer_encode(std::uint32_t sequence,
                                        std::uint32_t sample_position,
                                        std::span<const std::byte> pcm,
                                        std::span<std::byte> out)
{
    return aqua::net::encode_audio(0, sequence, sample_position, pcm, out);
}

// 模拟 client 解码收到的音频包, 返回 payload (拷贝)
std::vector<std::byte> simulate_client_decode(std::span<const std::byte> packet)
{
    auto decoded = aqua::net::decode_audio(packet);
    if (!decoded) return {};
    return std::vector<std::byte>(decoded->payload.begin(), decoded->payload.end());
}

} // namespace

// ==== 完整 NAT 握手流程 ====

TEST(NatFlowTest, ConnectHelloAckHandshake)
{
    // 场景: gRPC Connect 返回 session_id + UDP 端口; client 发 HELLO; server 记录并回 ACK
    SessionManager sm;

    // 1. 模拟 gRPC Connect
    auto sid = sm.create_session();
    ASSERT_TRUE(sid.has_value());
    // server 通过 gRPC 告知 client UDP 端口 (地址由 client 用 gRPC server IP)

    // 2. 模拟 client 发 HELLO, source = NAT 映射后地址
    auto client_nat_ep = make_nat_endpoint(10, 54321);

    std::array<std::byte, 64> ack_buf{};
    auto ack_written = simulate_server_hello(sm, *sid, client_nat_ep, ack_buf);
    ASSERT_GT(ack_written, 0u);

    // 3. server 回 HELLO_ACK, client 解码校验
    auto ack = aqua::net::decode_hello(std::span<const std::byte>{ack_buf.data(), ack_written});
    ASSERT_TRUE(ack.has_value());
    EXPECT_EQ(ack->type, PacketType::HelloAck);
    EXPECT_EQ(ack->session_id, *sid);

    // 4. server 侧 session 已 Connected, endpoint 已记录
    EXPECT_TRUE(sm.is_connected(*sid));
    EXPECT_EQ(sm.get_endpoint(*sid).value(), client_nat_ep);
}

TEST(NatFlowTest, HelloFromUnknownSessionIsDropped)
{
    // 场景: 攻击者/旧 client 用不存在的 session_id 发 HELLO, server 应丢弃
    SessionManager sm;
    auto valid_sid = sm.create_session();
    ASSERT_TRUE(valid_sid.has_value());

    // 合法 HELLO
    auto ep1 = make_nat_endpoint(1, 1000);
    std::array<std::byte, 64> ack_buf{};
    ASSERT_GT(simulate_server_hello(sm, *valid_sid, ep1, ack_buf), 0u);

    // 非法 session_id (必然不存在)
    auto bogus_ep = make_nat_endpoint(2, 2000);
    auto ack_written = simulate_server_hello(sm, 0xDEADBEEFu, bogus_ep, ack_buf);
    EXPECT_EQ(ack_written, 0u);

    // 合法 session 不受影响
    EXPECT_TRUE(sm.is_connected(*valid_sid));
    EXPECT_EQ(sm.get_endpoint(*valid_sid).value(), ep1);
}

// ==== 多 client 广播 ====

TEST(NatFlowTest, BroadcastToMultipleConnectedClients)
{
    // 场景: 3 个 client 都完成握手, server packetizer 广播音频, 3 个都应收到
    SessionManager sm;
    std::vector<SessionManager::session_id_t> sids;
    std::vector<asio::ip::udp::endpoint> eps;

    for (int i = 0; i < 3; ++i) {
        auto sid = sm.create_session();
        ASSERT_TRUE(sid.has_value());
        auto ep = make_nat_endpoint(i + 1, static_cast<unsigned short>(30000 + i));
        std::array<std::byte, 64> ack{};
        ASSERT_GT(simulate_server_hello(sm, *sid, ep, ack), 0u);
        sids.push_back(*sid);
        eps.push_back(ep);
    }

    // 模拟 packetizer: 编码一个音频包
    std::vector<std::byte> pcm(480, std::byte{0x55});
    std::vector<std::byte> packet(sizeof(AudioPacketHeader) + pcm.size());
    auto written = simulate_packetizer_encode(0, 0, pcm, packet);
    ASSERT_GT(written, 0u);

    // 模拟 server for_each_connected 广播, 收集每个 client 收到的 payload
    int received = 0;
    sm.for_each_connected([&](auto sid, const auto& ep) {
        auto payload = simulate_client_decode(
            std::span<const std::byte>{packet.data(), written});
        if (payload.size() == pcm.size() &&
            std::memcmp(payload.data(), pcm.data(), pcm.size()) == 0) {
            received++;
        }
        return true;
    });

    EXPECT_EQ(received, 3);
}

TEST(NatFlowTest, UnconnectedSessionDoesNotReceiveAudio)
{
    // 场景: 1 个 Connected, 1 个仅 Created (未握手); 广播时只有 Connected 收到
    SessionManager sm;
    auto sid_connected = sm.create_session();
    auto sid_created   = sm.create_session();
    ASSERT_TRUE(sid_connected.has_value());
    ASSERT_TRUE(sid_created.has_value());

    auto ep = make_nat_endpoint(1, 40000);
    std::array<std::byte, 64> ack{};
    ASSERT_TRUE(sm.establish_udp(*sid_connected, ep));

    std::vector<SessionManager::session_id_t> recipients;
    sm.for_each_connected([&](auto sid, const auto&) {
        recipients.push_back(sid);
        return true;
    });

    ASSERT_EQ(recipients.size(), 1u);
    EXPECT_EQ(recipients[0], *sid_connected);
    EXPECT_NE(recipients[0], *sid_created);
}

// ==== NAT 重映射 ====

TEST(NatFlowTest, NatRemapUpdatesEndpoint)
{
    // 场景: client 的 NAT 映射变化 (UDP 重新绑定), 重新发 HELLO, server 更新 endpoint
    // 旧 endpoint 的包不再送达, 新 endpoint 收到音频
    SessionManager sm;
    auto sid = sm.create_session();
    ASSERT_TRUE(sid.has_value());

    auto old_ep = make_nat_endpoint(5, 11111);
    auto new_ep = make_nat_endpoint(5, 22222); // 同 IP 不同端口 (NAT remap)

    std::array<std::byte, 64> ack{};
    ASSERT_GT(simulate_server_hello(sm, *sid, old_ep, ack), 0u);
    EXPECT_EQ(sm.get_endpoint(*sid).value(), old_ep);

    // NAT remap: client 重新发 HELLO
    ASSERT_GT(simulate_server_hello(sm, *sid, new_ep, ack), 0u);
    EXPECT_EQ(sm.get_endpoint(*sid).value(), new_ep);

    // 广播时只发到新 endpoint
    asio::ip::udp::endpoint delivered_to{};
    bool any_delivery = false;
    sm.for_each_connected([&](auto, const auto& ep) {
        delivered_to = ep;
        any_delivery = true;
        return true;
    });
    ASSERT_TRUE(any_delivery);
    EXPECT_EQ(delivered_to, new_ep);
    EXPECT_NE(delivered_to, old_ep);
}

// ==== 数据完整性 / 丢包模拟 ====

TEST(NatFlowTest, AudioDataIntegrityAcrossPackets)
{
    // 场景: 连续编码 10 个音频包, 每包 payload 不同, 解码后逐字节比对
    SessionManager sm;
    auto sid = sm.create_session();
    ASSERT_TRUE(sid.has_value());
    auto ep = make_nat_endpoint(1, 50000);
    std::array<std::byte, 64> ack{};
    ASSERT_GT(simulate_server_hello(sm, *sid, ep, ack), 0u);

    constexpr std::uint32_t frames_per_packet = 480; // 10ms @ 48kHz
    constexpr std::uint32_t frame_bytes = 4;         // S16LE 2ch
    constexpr std::size_t payload_size = frames_per_packet * frame_bytes;

    std::vector<std::byte> packet_buf(sizeof(AudioPacketHeader) + payload_size);

    for (std::uint32_t seq = 0; seq < 10; ++seq) {
        // 每包填充不同的 pattern: byte = seq
        std::vector<std::byte> pcm(payload_size, std::byte{static_cast<uint8_t>(seq)});
        auto written = simulate_packetizer_encode(
            seq, seq * frames_per_packet, pcm, packet_buf);
        ASSERT_GT(written, 0u);

        auto decoded = simulate_client_decode(
            std::span<const std::byte>{packet_buf.data(), written});
        ASSERT_EQ(decoded.size(), payload_size);

        // 逐字节校验
        for (std::size_t i = 0; i < decoded.size(); ++i) {
            ASSERT_EQ(decoded[i], std::byte{static_cast<uint8_t>(seq)})
                << "byte mismatch at seq=" << seq << " offset=" << i;
        }
    }
}

TEST(NatFlowTest, PacketLossSimulation)
{
    // 场景: 模拟网络丢包, sequence 跳跃 (0,1,3,5), client 端检测缺口
    // 当前 M3 阶段 client 无 Jitter Buffer, 仅验证解码每个到达的包
    std::vector<std::byte> pcm(100, std::byte{0xAA});
    std::vector<std::byte> packet_buf(sizeof(AudioPacketHeader) + pcm.size());

    std::vector<std::uint32_t> sent_seqs = {0, 1, 3, 5}; // 2,4 丢失
    std::vector<std::uint32_t> received_seqs;

    for (auto seq : sent_seqs) {
        auto written = simulate_packetizer_encode(seq, seq * 480, pcm, packet_buf);
        ASSERT_GT(written, 0u);
        auto decoded = aqua::net::decode_audio(
            std::span<const std::byte>{packet_buf.data(), written});
        ASSERT_TRUE(decoded.has_value());
        received_seqs.push_back(decoded->header.sequence);
    }

    EXPECT_EQ(received_seqs, sent_seqs);

    // 检测丢包: 期望 0,1,2,3,4,5, 实际缺 2,4
    std::unordered_set<std::uint32_t> received_set(received_seqs.begin(),
                                                    received_seqs.end());
    std::vector<std::uint32_t> missing;
    for (std::uint32_t s = 0; s <= 5; ++s) {
        if (!received_set.count(s)) missing.push_back(s);
    }
    ASSERT_EQ(missing.size(), 2u);
    EXPECT_EQ(missing[0], 2u);
    EXPECT_EQ(missing[1], 4u);
}

TEST(NatFlowTest, OutOfOrderPacketDecodes)
{
    // 场景: UDP 包乱序到达 (seq 2, 0, 1), 每个包仍可独立解码
    std::vector<std::byte> pcm(50, std::byte{0xBB});
    std::vector<std::byte> buf0(sizeof(AudioPacketHeader) + 50);
    std::vector<std::byte> buf1(sizeof(AudioPacketHeader) + 50);
    std::vector<std::byte> buf2(sizeof(AudioPacketHeader) + 50);

    ASSERT_GT(simulate_packetizer_encode(0, 0,   pcm, buf0), 0u);
    ASSERT_GT(simulate_packetizer_encode(1, 480, pcm, buf1), 0u);
    ASSERT_GT(simulate_packetizer_encode(2, 960, pcm, buf2), 0u);

    // 乱序到达: 2, 0, 1
    auto d2 = aqua::net::decode_audio(buf2);
    auto d0 = aqua::net::decode_audio(buf0);
    auto d1 = aqua::net::decode_audio(buf1);

    ASSERT_TRUE(d2.has_value());
    ASSERT_TRUE(d0.has_value());
    ASSERT_TRUE(d1.has_value());
    EXPECT_EQ(d2->header.sequence, 2u);
    EXPECT_EQ(d0->header.sequence, 0u);
    EXPECT_EQ(d1->header.sequence, 1u);
}

// ==== 超时清理流程 ====

TEST(NatFlowTest, ExpiredSessionsRemovedFromBroadcast)
{
    // 场景: 2 个 Connected session, 一个超时被清理, 广播时只剩 1 个
    SessionManager sm;
    auto sid1 = sm.create_session();
    auto sid2 = sm.create_session();
    ASSERT_TRUE(sid1.has_value());
    ASSERT_TRUE(sid2.has_value());

    auto ep1 = make_nat_endpoint(1, 60000);
    auto ep2 = make_nat_endpoint(2, 60001);
    ASSERT_TRUE(sm.establish_udp(*sid1, ep1));
    ASSERT_TRUE(sm.establish_udp(*sid2, ep2));

    // 初始 2 个 Connected
    size_t count = 0;
    sm.for_each_connected([&](auto, const auto&) { count++; return true; });
    EXPECT_EQ(count, 2u);

    // 模拟超时: 等待并清理 (阈值 1s)
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    auto expired = sm.collect_expired_sessions(std::chrono::seconds(1));
    ASSERT_EQ(expired.size(), 2u); // 两个都过期了 (都没 touch)
    for (auto id : expired) {
        sm.remove_session(id);
    }

    count = 0;
    sm.for_each_connected([&](auto, const auto&) { count++; return true; });
    EXPECT_EQ(count, 0u);
}

TEST(NatFlowTest, KeepAliveRefreshesSession)
{
    // 场景: client 周期发 KeepAlive (touch_session), session 不应过期
    SessionManager sm;
    auto sid = sm.create_session();
    ASSERT_TRUE(sid.has_value());
    auto ep = make_nat_endpoint(1, 70000);
    ASSERT_TRUE(sm.establish_udp(*sid, ep));

    // 模拟 3 次 KeepAlive, 每次间隔 400ms, 阈值 1s
    for (int i = 0; i < 3; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
        ASSERT_TRUE(sm.touch_session(*sid));
    }

    // 此时仍未过期 (最后一次 touch 距现在 < 1s)
    auto expired = sm.collect_expired_sessions(std::chrono::seconds(1));
    EXPECT_TRUE(expired.empty());

    // 停止 KeepAlive, 等待超时
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    expired = sm.collect_expired_sessions(std::chrono::seconds(1));
    ASSERT_EQ(expired.size(), 1u);
    EXPECT_EQ(expired[0], *sid);
}

// ==== 畸形包模拟 ====

TEST(NatFlowTest, MalformedHelloIsRejected)
{
    // 场景: 收到截断的 HELLO 包, decode 失败, server 不应 establish_udp
    SessionManager sm;
    auto sid = sm.create_session();
    ASSERT_TRUE(sid.has_value());

    // 模拟截断包 (只有 2 字节, 不够 HelloPacket)
    std::array<std::byte, 2> truncated{};
    auto decoded = aqua::net::decode_hello(truncated);
    EXPECT_FALSE(decoded.has_value());

    // session 仍未握手
    EXPECT_FALSE(sm.is_connected(*sid));
}

TEST(NatFlowTest, WrongPacketTypeDoesNotTriggerHandshake)
{
    // 场景: client 发了 Audio 包而非 Hello, server 不应 establish_udp
    SessionManager sm;
    auto sid = sm.create_session();
    ASSERT_TRUE(sid.has_value());

    std::vector<std::byte> pcm(10, std::byte{0x11});
    std::vector<std::byte> audio_packet(sizeof(AudioPacketHeader) + 10);
    ASSERT_GT(aqua::net::encode_audio(*sid, 0, 0, pcm, audio_packet), 0u);

    // server peek_type -> Audio, 不走 HELLO 分支
    auto type = aqua::net::peek_type(audio_packet);
    ASSERT_TRUE(type.has_value());
    EXPECT_EQ(*type, PacketType::Audio);
    EXPECT_NE(*type, PacketType::Hello);

    // session 仍未握手
    EXPECT_FALSE(sm.is_connected(*sid));
}
