#include "aqua/audio/packetizer/audio_packetizer.h"

#include "aqua/audio/audio_format.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <vector>

namespace {

using aqua::audio::AudioPacketizer;

struct Captured {
    std::uint64_t sequence;
    std::vector<std::byte> pcm;
};

void push_capture(AudioPacketizer& pkt, std::span<const std::byte> pcm, std::vector<Captured>& out)
{
    const auto handler = [&out](const aqua::audio::AudioFrame& frame) noexcept {
        out.push_back(Captured { frame.sequence,
            std::vector<std::byte>(frame.data.begin(), frame.data.end()) });
    };
    pkt.push(pcm, handler);
}

std::vector<std::byte> bytes_of(std::initializer_list<int> vals)
{
    std::vector<std::byte> v;
    v.reserve(vals.size());
    for (int x : vals) {
        v.push_back(static_cast<std::byte>(x));
    }
    return v;
}

TEST(AudioPacketizerBoundaryTest, SingleFrameSingleByte)
{
    AudioPacketizer pkt(1, 1); // F=1，1 字节/帧
    std::vector<Captured> out;
    push_capture(pkt, bytes_of({ 42 }), out);

    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].sequence, 0u);
    ASSERT_EQ(out[0].pcm.size(), 1u);
    EXPECT_EQ(std::to_integer<std::uint8_t>(out[0].pcm[0]), 42u);
}

TEST(AudioPacketizerBoundaryTest, EmptyPushEmitsNothing)
{
    AudioPacketizer pkt(4, 1);
    std::vector<Captured> out;
    push_capture(pkt, std::span<const std::byte> {}, out);
    EXPECT_TRUE(out.empty());
    EXPECT_EQ(pkt.frames_emitted(), 0u);
}

TEST(AudioPacketizerBoundaryTest, ExactFrameBoundaryNoRemainder)
{
    AudioPacketizer pkt(4, 1);
    std::vector<Captured> out;
    push_capture(pkt, bytes_of({ 1, 2, 3, 4 }), out); // 恰好 4 帧
    ASSERT_EQ(out.size(), 1u);

    push_capture(pkt, bytes_of({ 5 }), out); // 1 帧，不足 4
    EXPECT_EQ(out.size(), 1u); // 仍只有 1
    EXPECT_EQ(pkt.frames_emitted(), 1u);
}

TEST(AudioPacketizerBoundaryTest, FrameCountForBudgetBoundaries)
{
    // frame_bytes = 1（PCM_U8 1ch）
    aqua::audio::AudioFormat u8;
    u8.encoding = aqua::audio::AudioEncoding::PCM_U8;
    u8.channels = 1;
    u8.sample_rate = 48000;
    EXPECT_EQ(aqua::audio::frame_count_for_budget(u8, 0), 0u);
    EXPECT_EQ(aqua::audio::frame_count_for_budget(u8, 1), 1u);
    EXPECT_EQ(aqua::audio::frame_count_for_budget(u8, 100), 100u);

    // frame_bytes = 8（F32LE 2ch）
    aqua::audio::AudioFormat f32;
    f32.encoding = aqua::audio::AudioEncoding::PCM_F32LE;
    f32.channels = 2;
    f32.sample_rate = 48000;
    EXPECT_EQ(aqua::audio::frame_count_for_budget(f32, 7), 0u); // 不足一帧
    EXPECT_EQ(aqua::audio::frame_count_for_budget(f32, 8), 1u);
    EXPECT_EQ(aqua::audio::frame_count_for_budget(f32, 1472), 184u);
    EXPECT_EQ(aqua::audio::frame_count_for_budget(f32, 1443), 180u); // IPv6-safe（CLI auto-F 用）

    // 非法格式 → 0
    aqua::audio::AudioFormat bad;
    EXPECT_EQ(aqua::audio::frame_count_for_budget(bad, 1000), 0u);
}

TEST(AudioPacketizerBoundaryTest, RejectsUnalignedInput)
{
    AudioPacketizer pkt(2, 2); // F=2 帧，2 字节/帧 → 每 AudioFrame 4 字节
    std::vector<Captured> out;

    // 3 字节不是 2 的整数倍 → 丢弃，不污染 pending。
    push_capture(pkt, bytes_of({ 1, 2, 3 }), out);
    EXPECT_TRUE(out.empty());
    EXPECT_EQ(pkt.frames_emitted(), 0u);

    // 之后推对齐的 4 字节 → 恰好产出 1 帧，且不含之前被丢弃的 3 字节。
    push_capture(pkt, bytes_of({ 4, 5, 6, 7 }), out);
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].pcm, bytes_of({ 4, 5, 6, 7 }));
}

TEST(AudioPacketizerBoundaryTest, ConfigValidation)
{
    EXPECT_TRUE(AudioPacketizer::is_valid_config(1, 1));
    EXPECT_FALSE(AudioPacketizer::is_valid_config(0, 1));
    EXPECT_FALSE(AudioPacketizer::is_valid_config(1, 0));
    EXPECT_FALSE(AudioPacketizer::is_valid_config(0, 0));

    AudioPacketizer bad(0, 0);
    EXPECT_FALSE(bad.valid());
    AudioPacketizer good(4, 1);
    EXPECT_TRUE(good.valid());
}

} // namespace
