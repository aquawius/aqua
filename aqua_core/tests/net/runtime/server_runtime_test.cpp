#include "aqua/runtime/server_runtime.h"

#include "aqua/net/udp/network_frame.h"

#include <gtest/gtest.h>
#include <asio.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace {

using aqua::runtime::ServerRuntime;
using aqua::runtime::ServerRuntimeConfig;

std::uint16_t find_free_udp_port()
{
    asio::io_context io;
    asio::ip::udp::socket s(io);
    s.open(asio::ip::udp::v4());
    s.bind(asio::ip::udp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    return s.local_endpoint().port();
}

ServerRuntimeConfig make_config(std::uint16_t port)
{
    ServerRuntimeConfig cfg;
    cfg.format.encoding = aqua::audio::AudioEncoding::PCM_F32LE;
    cfg.format.channels = 1;
    cfg.format.sample_rate = 48000;
    cfg.frames_per_slot = 4;
    cfg.udp_bind_ip = "127.0.0.1";
    cfg.udp_port = port;
    return cfg;
}

TEST(ServerRuntimeTest, PushPcmBroadcastsAudioToConnectedSession)
{
    asio::io_context ioc;

    // 接收端：原始 socket 绑定 127.0.0.1:0（具体地址，非 unspecified）。
    asio::ip::udp::socket receiver(ioc);
    receiver.open(asio::ip::udp::v4());
    receiver.bind(asio::ip::udp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    const auto receiver_ep = receiver.local_endpoint();

    auto received = std::make_shared<std::vector<std::byte>>(256);
    auto received_len = std::make_shared<std::size_t>(0);
    asio::ip::udp::endpoint sender_ep;
    receiver.async_receive_from(asio::buffer(*received), sender_ep,
        [received, received_len](const asio::error_code& ec, std::size_t len) {
            if (!ec) {
                *received_len = len;
            }
        });

    auto rt = std::make_shared<ServerRuntime>(ioc, make_config(find_free_udp_port()));
    ASSERT_TRUE(rt->start());

    const auto id = rt->sessions().create_session();
    ASSERT_TRUE(id.has_value());
    ASSERT_TRUE(rt->sessions().establish_session(*id, receiver_ep));

    std::vector<std::byte> pcm(16, std::byte { 0xAB }); // 4 帧 × 4 字节（F32LE 1ch）
    rt->push_pcm(pcm);

    ioc.run_for(std::chrono::milliseconds(100));

    ASSERT_GT(*received_len, 0u);
    const std::span<const std::byte> packet(received->data(), *received_len);
    const auto nf = aqua::net::NetworkFrame::decode(packet);
    ASSERT_TRUE(nf.has_value());
    EXPECT_EQ(nf->type(), aqua::net::PacketType::Audio);
    EXPECT_EQ(nf->sequence(), 0u);
    ASSERT_EQ(nf->payload().size(), 16u);
    EXPECT_EQ(std::to_integer<std::uint8_t>(nf->payload()[0]), 0xABu);
    EXPECT_EQ(rt->frames_broadcast(), 1u);

    rt->stop();
    receiver.close();
}

TEST(ServerRuntimeTest, NoBroadcastWithoutConnectedSession)
{
    asio::io_context ioc;
    auto rt = std::make_shared<ServerRuntime>(ioc, make_config(find_free_udp_port()));
    ASSERT_TRUE(rt->start());

    std::vector<std::byte> pcm(16, std::byte { 0xCD });
    rt->push_pcm(pcm);
    EXPECT_EQ(rt->frames_broadcast(), 1u);

    rt->stop();
}

TEST(ServerRuntimeTest, ExpiredSessionIsRemoved)
{
    asio::io_context ioc;

    aqua::runtime::ServerRuntimeConfig cfg;
    cfg.format.encoding = aqua::audio::AudioEncoding::PCM_F32LE;
    cfg.format.channels = 1;
    cfg.format.sample_rate = 48000;
    cfg.frames_per_slot = 4;
    cfg.udp_bind_ip = "127.0.0.1";
    cfg.udp_port = find_free_udp_port();
    cfg.session_timeout = std::chrono::milliseconds(50);
    cfg.session_reap_interval = std::chrono::milliseconds(10);

    auto rt = std::make_shared<ServerRuntime>(ioc, cfg);
    ASSERT_TRUE(rt->start());

    const auto id = rt->sessions().create_session();
    ASSERT_TRUE(id.has_value());
    ASSERT_TRUE(rt->sessions().establish_session(
        *id, asio::ip::udp::endpoint(asio::ip::make_address("127.0.0.1"), 9999)));
    EXPECT_EQ(rt->sessions().session_count(), 1u);

    // 让 reap timer 跑一段，超时 session 应被移除。
    ioc.run_for(std::chrono::milliseconds(200));
    EXPECT_EQ(rt->sessions().session_count(), 0u);

    rt->stop();
}

} // namespace
