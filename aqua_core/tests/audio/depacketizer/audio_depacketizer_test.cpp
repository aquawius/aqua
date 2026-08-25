#include "aqua/audio/depacketizer/audio_depacketizer.h"

#include "aqua/audio/audio_format.h"
#include "aqua/audio/buffer/jitter_buffer.h"
#include "aqua/net/udp/udp_packet.h"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace {

using aqua::audio::AudioDepacketizer;
using aqua::audio::AudioEncoding;
using aqua::audio::AudioFormat;
using aqua::audio::JitterBuffer;
using aqua::audio::JitterBufferConfig;

constexpr std::uint32_t kFrameBytes = 4; // PCM_F32LE 单声道

AudioFormat make_format()
{
    return AudioFormat { AudioEncoding::PCM_F32LE, 1, 48000 };
}

JitterBufferConfig make_config(std::uint32_t slots, std::uint32_t frames_per_slot)
{
    JitterBufferConfig c;
    c.capacity_slots = slots;
    c.format = make_format();
    c.frames_per_slot = frames_per_slot;
    return c;
}

std::vector<std::byte> make_payload(std::uint32_t frames, std::uint8_t fill)
{
    std::vector<std::byte> d(static_cast<std::size_t>(frames) * kFrameBytes);
    std::fill(d.begin(), d.end(), static_cast<std::byte>(fill));
    return d;
}

TEST(AudioDepacketizerTest, AudioDatagramPushesToBuffer)
{
    auto jb = JitterBuffer::create(make_config(10, 4));
    ASSERT_TRUE(jb.has_value());
    AudioDepacketizer dep(**jb, 4);

    const auto datagram = aqua::net::encode_audio_packet(100, make_payload(4, 42));
    EXPECT_TRUE(dep.handle_datagram(datagram));
    EXPECT_EQ((*jb)->used_slots(), 1u);
}

TEST(AudioDepacketizerTest, NonAudioDatagramIgnored)
{
    auto jb = JitterBuffer::create(make_config(10, 4));
    ASSERT_TRUE(jb.has_value());
    AudioDepacketizer dep(**jb, 4);

    const auto hello = aqua::net::encode_hello_packet(0x12345678u);
    EXPECT_FALSE(dep.handle_datagram(hello));
    EXPECT_EQ((*jb)->used_slots(), 0u);
}

TEST(AudioDepacketizerTest, MalformedDatagramRejected)
{
    auto jb = JitterBuffer::create(make_config(10, 4));
    ASSERT_TRUE(jb.has_value());
    AudioDepacketizer dep(**jb, 4);

    const std::array<std::byte, 4> short_packet {}; // 不足 Audio header
    EXPECT_FALSE(dep.handle_datagram(short_packet));
    EXPECT_EQ((*jb)->used_slots(), 0u);
}

TEST(AudioDepacketizerTest, WrongPayloadSizeRejectedByBuffer)
{
    auto jb = JitterBuffer::create(make_config(10, 4));
    ASSERT_TRUE(jb.has_value());
    AudioDepacketizer dep(**jb, 4);

    // payload 只有 3 帧（12 字节）而非 4 帧（16 字节）→ JB 拒收。
    const auto datagram = aqua::net::encode_audio_packet(100, make_payload(3, 42));
    EXPECT_FALSE(dep.handle_datagram(datagram));
    EXPECT_EQ((*jb)->used_slots(), 0u);
}

TEST(AudioDepacketizerTest, RoundTripThroughCodecAndBuffer)
{
    auto jb = JitterBuffer::create(make_config(10, 4));
    ASSERT_TRUE(jb.has_value());
    AudioDepacketizer dep(**jb, 4);

    // 推 6 帧（seq 100..105），fill = seq+1。
    for (std::uint64_t s = 100; s <= 105; ++s) {
        const auto dgram = aqua::net::encode_audio_packet(
            s, make_payload(4, static_cast<std::uint8_t>(s + 1)));
        EXPECT_TRUE(dep.handle_datagram(dgram));
    }

    // lead=6=target → pull 建立 anchor 并输出 seq 100（fill=101）。
    std::vector<std::byte> out(4 * kFrameBytes);
    const auto r = (*jb)->pull(out);
    EXPECT_EQ(r.frames_filled, 4u);
    EXPECT_EQ(std::to_integer<std::uint8_t>(out[0]), 101u);
}

} // namespace
