#include "aqua/runtime/client_runtime.h"

#include "aqua/net/udp/udp_packet.h"

#include <gtest/gtest.h>
#include <asio.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace {

using aqua::runtime::ClientRuntime;
using aqua::runtime::ClientRuntimeConfig;

constexpr std::uint32_t kFrameBytes = 4; // PCM_F32LE 单声道

aqua::audio::AudioFormat make_format()
{
    return aqua::audio::AudioFormat { aqua::audio::AudioEncoding::PCM_F32LE, 1, 48000 };
}

ClientRuntimeConfig make_config()
{
    ClientRuntimeConfig cfg;
    cfg.jitter_buffer_slots = 10; // target 60% = 6 槽，便于测试
    return cfg;
}

std::vector<std::byte> make_payload(std::uint32_t frames, std::uint8_t fill)
{
    std::vector<std::byte> d(static_cast<std::size_t>(frames) * kFrameBytes);
    std::fill(d.begin(), d.end(), static_cast<std::byte>(fill));
    return d;
}

TEST(ClientRuntimeTest, RoutesAudioDatagramToJitterBuffer)
{
    asio::io_context ioc;
    auto rt = std::make_shared<ClientRuntime>(ioc, make_config());
    ASSERT_TRUE(rt->setup_playback(make_format(), 4));

    const auto dgram = aqua::net::encode_audio_packet(100, make_payload(4, 42));
    rt->handle_datagram(asio::ip::udp::endpoint {}, dgram);

    ASSERT_NE(rt->jitter_buffer(), nullptr);
    EXPECT_EQ(rt->jitter_buffer()->used_slots(), 1u);
}

TEST(ClientRuntimeTest, IgnoresNonAudioDatagram)
{
    asio::io_context ioc;
    auto rt = std::make_shared<ClientRuntime>(ioc, make_config());
    ASSERT_TRUE(rt->setup_playback(make_format(), 4));

    const auto hello = aqua::net::encode_hello_packet(0x12345678u);
    rt->handle_datagram(asio::ip::udp::endpoint {}, hello);
    EXPECT_EQ(rt->jitter_buffer()->used_slots(), 0u);
}

TEST(ClientRuntimeTest, PullPlaybackReturnsBufferedFrames)
{
    asio::io_context ioc;
    auto rt = std::make_shared<ClientRuntime>(ioc, make_config());
    ASSERT_TRUE(rt->setup_playback(make_format(), 4));

    // 推 6 帧（lead=6=target，N=10）→ 锚定后 pull 出 seq 100（fill=101）。
    for (std::uint64_t s = 100; s <= 105; ++s) {
        const auto dgram = aqua::net::encode_audio_packet(
            s, make_payload(4, static_cast<std::uint8_t>(s + 1)));
        rt->handle_datagram(asio::ip::udp::endpoint {}, dgram);
    }

    std::vector<std::byte> out(4 * kFrameBytes);
    EXPECT_EQ(rt->pull_playback(out), 4u);
    EXPECT_EQ(std::to_integer<std::uint8_t>(out[0]), 101u);
}

} // namespace
