#include "aqua/audio/audio_format.h"
#include "aqua/audio/audio_frame.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace {

using aqua::audio::AudioEncoding;
using aqua::audio::AudioFormat;
using aqua::audio::AudioFrame;

AudioFormat make_valid_format()
{
    return AudioFormat {
        AudioEncoding::PCM_F32LE,
        2,
        48000,
    };
}

TEST(AudioFormatTest, ValidFormatProperties)
{
    const auto fmt = make_valid_format();
    EXPECT_TRUE(fmt.is_valid());
    EXPECT_EQ(fmt.bytes_per_sample(), 4u);
    EXPECT_EQ(fmt.frame_bytes(), 8u);
}

TEST(AudioFormatTest, BytesForFrames)
{
    const auto fmt = make_valid_format();
    EXPECT_EQ(fmt.bytes_for_frames(144), 1152u);
    EXPECT_EQ(fmt.bytes_for_frames(0), 0u);
}

TEST(AudioFormatTest, FrameAlignment)
{
    const auto fmt = make_valid_format();
    EXPECT_TRUE(fmt.is_frame_aligned(1152));
    EXPECT_FALSE(fmt.is_frame_aligned(1153));
}

TEST(AudioFormatTest, FramesFromBytes)
{
    const auto fmt = make_valid_format();
    ASSERT_TRUE(fmt.frames_from_bytes(1152).has_value());
    EXPECT_EQ(*fmt.frames_from_bytes(1152), 144u);

    // 非完整 frame 无法转换。
    EXPECT_EQ(fmt.frames_from_bytes(1153), std::nullopt);

    // 0 字节是合法输入，对应 0 frames。
    ASSERT_TRUE(fmt.frames_from_bytes(0).has_value());
    EXPECT_EQ(*fmt.frames_from_bytes(0), 0u);
}

TEST(AudioFrameTest, WellFormedFrame)
{
    const auto fmt = make_valid_format();
    std::byte storage[1152] { };
    AudioFrame frame {
        1, // sequence
        123, // timestamp_ns
        144, // frame_count
        std::span<const std::byte>(storage),
    };
    EXPECT_TRUE(frame.is_well_formed(fmt));
}

TEST(AudioFrameTest, MalformedFrameRejected)
{
    const auto fmt = make_valid_format();
    std::byte storage[1152] { };
    // frame_count 与 data 长度不匹配：143 frames 需要 1144 字节。
    AudioFrame bad_frame {
        2,
        0,
        143,
        std::span<const std::byte>(storage),
    };
    EXPECT_FALSE(bad_frame.is_well_formed(fmt));
}

} // namespace
