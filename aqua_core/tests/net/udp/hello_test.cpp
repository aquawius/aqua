#include "aqua/session/hello.h"

#include "aqua/net/udp/udp_packet.h"

#include <gtest/gtest.h>
#include <asio.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace {

using aqua::HelloResponder;
using aqua::HelloSender;
using aqua::SessionManager;

asio::ip::udp::endpoint make_ep(const char* addr, std::uint16_t port)
{
    return asio::ip::udp::endpoint(asio::ip::make_address(addr), port);
}

struct AckCapture {
    asio::ip::udp::endpoint target;
    std::uint32_t session_id = 0;
    bool called = false;
};

void capture_ack(void* ud, const asio::ip::udp::endpoint& target,
    std::span<const std::byte> ack) noexcept
{
    auto* c = static_cast<AckCapture*>(ud);
    c->called = true;
    c->target = target;
    (void)aqua::net::decode_hello_packet(ack, c->session_id);
}

struct PacketCapture {
    std::vector<std::byte> packet;
};

void capture_packet(void* ud, std::span<const std::byte> packet) noexcept
{
    static_cast<PacketCapture*>(ud)->packet.assign(packet.begin(), packet.end());
}

TEST(HelloResponderTest, EstablishesSessionAndRepliesAck)
{
    SessionManager sessions;
    const auto id = sessions.create_session();
    ASSERT_TRUE(id.has_value());

    AckCapture cap;
    HelloResponder resp(sessions, capture_ack, &cap);

    const auto hello = aqua::net::encode_hello_packet(*id);
    const auto sender = make_ep("127.0.0.1", 9999);

    EXPECT_TRUE(resp.handle(sender, hello));
    EXPECT_TRUE(sessions.is_connected(*id));
    EXPECT_TRUE(cap.called);
    EXPECT_EQ(cap.target, sender);
    EXPECT_EQ(cap.session_id, *id);
}

TEST(HelloResponderTest, IgnoresNonHelloDatagram)
{
    SessionManager sessions;
    const auto id = sessions.create_session();
    ASSERT_TRUE(id.has_value());

    AckCapture cap;
    HelloResponder resp(sessions, capture_ack, &cap);

    const auto audio = aqua::net::encode_audio_packet(1, std::span<const std::byte> {});
    EXPECT_FALSE(resp.handle(make_ep("127.0.0.1", 9999), audio));
    EXPECT_FALSE(sessions.is_connected(*id));
    EXPECT_FALSE(cap.called);
}

TEST(HelloResponderTest, IgnoresUnknownSession)
{
    SessionManager sessions;
    AckCapture cap;
    HelloResponder resp(sessions, capture_ack, &cap);

    const auto hello = aqua::net::encode_hello_packet(0x12345678u); // 未创建的 session
    EXPECT_FALSE(resp.handle(make_ep("127.0.0.1", 9999), hello));
    EXPECT_FALSE(sessions.is_connected(0x12345678u));
    EXPECT_FALSE(cap.called);
}

TEST(HelloResponderTest, IgnoresMalformedHello)
{
    SessionManager sessions;
    AckCapture cap;
    HelloResponder resp(sessions, capture_ack, &cap);

    const std::vector<std::byte> short_packet(3); // 不足 Hello 长度
    EXPECT_FALSE(resp.handle(make_ep("127.0.0.1", 9999), short_packet));
    EXPECT_FALSE(cap.called);
}

TEST(HelloSenderTest, SendsHelloWithSessionId)
{
    PacketCapture cap;
    HelloSender sender(0xCAFEBABEu, capture_packet, &cap);
    sender.send();

    ASSERT_FALSE(cap.packet.empty());
    EXPECT_EQ(aqua::net::decode_packet_type(cap.packet), aqua::net::PacketType::Hello);
    std::uint32_t sid = 0;
    ASSERT_TRUE(aqua::net::decode_hello_packet(cap.packet, sid));
    EXPECT_EQ(sid, 0xCAFEBABEu);
}

} // namespace
