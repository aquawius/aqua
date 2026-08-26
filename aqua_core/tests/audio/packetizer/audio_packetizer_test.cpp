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
    AudioPacketizer::FrameHandler handler =
        [&out](std::uint64_t seq, std::span<const std::byte> data) noexcept {
            out.push_back(Captured { seq, std::vector<std::byte>(data.begin(), data.end()) });
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

TEST(AudioPacketizerTest, ExactChunkEmitsOneFrame)
{
    AudioPacketizer pkt(4, 1); // F=4 sample frames，1 字节/帧
    std::vector<Captured> out;
    const auto pcm = bytes_of({ 1, 2, 3, 4 });
    push_capture(pkt, pcm, out);

    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].sequence, 0u);
    EXPECT_EQ(out[0].pcm, pcm);
    EXPECT_EQ(pkt.frames_emitted(), 1u);
}

TEST(AudioPacketizerTest, PartialAccumulatesAcrossPushes)
{
    AudioPacketizer pkt(4, 1);
    std::vector<Captured> out;

    push_capture(pkt, bytes_of({ 1, 2, 3 }), out); // 3 帧，不足 4
    EXPECT_TRUE(out.empty());

    push_capture(pkt, bytes_of({ 4, 5, 6 }), out); // +3 = 6，凑出 1 帧，余 2 帧
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].pcm, bytes_of({ 1, 2, 3, 4 }));

    push_capture(pkt, bytes_of({ 7, 8 }), out); // +2 = 4，凑出第 2 帧
    ASSERT_EQ(out.size(), 2u);
    EXPECT_EQ(out[1].sequence, 1u);
    EXPECT_EQ(out[1].pcm, bytes_of({ 5, 6, 7, 8 }));
    EXPECT_EQ(pkt.frames_emitted(), 2u);
}

TEST(AudioPacketizerTest, MultipleFramesPerPush)
{
    AudioPacketizer pkt(4, 1);
    std::vector<Captured> out;
    push_capture(pkt, bytes_of({ 1, 2, 3, 4, 5, 6, 7, 8 }), out);

    ASSERT_EQ(out.size(), 2u);
    EXPECT_EQ(out[0].sequence, 0u);
    EXPECT_EQ(out[1].sequence, 1u);
    EXPECT_EQ(out[0].pcm, bytes_of({ 1, 2, 3, 4 }));
    EXPECT_EQ(out[1].pcm, bytes_of({ 5, 6, 7, 8 }));
}

TEST(AudioPacketizerTest, ResetClearsPendingAndSequence)
{
    AudioPacketizer pkt(4, 1);
    std::vector<Captured> out;
    push_capture(pkt, bytes_of({ 1, 2 }), out);
    pkt.reset();

    push_capture(pkt, bytes_of({ 9, 9, 9, 9 }), out);
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].sequence, 0u); // sequence 归零
    EXPECT_EQ(out[0].pcm, bytes_of({ 9, 9, 9, 9 })); // 之前的 {1,2} 被清掉
}

TEST(AudioPacketizerTest, MultiByteFrames)
{
    AudioPacketizer pkt(2, 2); // F=2 sample frames，2 字节/帧 → 每 AudioFrame 4 字节
    std::vector<Captured> out;
    push_capture(pkt, bytes_of({ 1, 2, 3, 4, 5, 6 }), out); // 6 字节 = 3 帧 → 1 帧 + 余 1 帧

    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].pcm, bytes_of({ 1, 2, 3, 4 }));

    push_capture(pkt, bytes_of({ 7, 8 }), out); // +1 帧 = 凑满第 2 帧
    ASSERT_EQ(out.size(), 2u);
    EXPECT_EQ(out[1].pcm, bytes_of({ 5, 6, 7, 8 }));
}

TEST(AudioPacketizerTest, FrameCountForBudget)
{
    aqua::audio::AudioFormat fmt;
    fmt.encoding = aqua::audio::AudioEncoding::PCM_F32LE;
    fmt.channels = 2;
    fmt.sample_rate = 48000;
    // frame_bytes = 8；budget 1472 → 184 sample frames。
    EXPECT_EQ(aqua::audio::frame_count_for_budget(fmt, 1472), 184u);
    EXPECT_EQ(aqua::audio::frame_count_for_budget(fmt, 0), 0u);
    EXPECT_EQ(aqua::audio::frame_count_for_budget(fmt, 7), 0u); // 不足一帧

    aqua::audio::AudioFormat bad; // 非法
    EXPECT_EQ(aqua::audio::frame_count_for_budget(bad, 1472), 0u);
}

} // namespace
