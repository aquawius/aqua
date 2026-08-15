#include <gtest/gtest.h>

#include "core/public/audio_format.h"

using aqua::AudioEncoding;
using aqua::AudioFormat;

TEST(AudioFormatTest, DefaultIsInvalid)
{
    AudioFormat fmt;
    EXPECT_FALSE(fmt.valid());
    EXPECT_EQ(fmt.encoding, AudioEncoding::Invalid);
    EXPECT_EQ(fmt.channels, 0u);
    EXPECT_EQ(fmt.sample_rate, 0u);
    EXPECT_EQ(fmt.bytes_per_sample(), 0u);
    EXPECT_EQ(fmt.frame_bytes(), 0u);
}

TEST(AudioFormatTest, ValidFormat)
{
    AudioFormat fmt { AudioEncoding::PcmS16LE, 2, 48000 };
    EXPECT_TRUE(fmt.valid());
    EXPECT_EQ(fmt.bytes_per_sample(), 2u);
    EXPECT_EQ(fmt.frame_bytes(), 4u);
}

TEST(AudioFormatTest, BytesPerSampleForEachEncoding)
{
    EXPECT_EQ((AudioFormat { AudioEncoding::PcmU8, 1, 8000 }).bytes_per_sample(), 1u);
    EXPECT_EQ((AudioFormat { AudioEncoding::PcmS16LE, 1, 8000 }).bytes_per_sample(), 2u);
    EXPECT_EQ((AudioFormat { AudioEncoding::PcmS24LE, 1, 8000 }).bytes_per_sample(), 3u);
    EXPECT_EQ((AudioFormat { AudioEncoding::PcmS32LE, 1, 8000 }).bytes_per_sample(), 4u);
    EXPECT_EQ((AudioFormat { AudioEncoding::PcmF32LE, 1, 8000 }).bytes_per_sample(), 4u);
}

TEST(AudioFormatTest, FrameBytesScalesWithChannels)
{
    AudioFormat fmt { AudioEncoding::PcmS16LE, 6, 48000 };
    EXPECT_EQ(fmt.bytes_per_sample(), 2u);
    EXPECT_EQ(fmt.frame_bytes(), 12u);

    AudioFormat stereo { AudioEncoding::PcmF32LE, 2, 48000 };
    EXPECT_EQ(stereo.frame_bytes(), 8u);
}

TEST(AudioFormatTest, InvalidEncodingRejected)
{
    AudioFormat fmt { AudioEncoding::Invalid, 2, 48000 };
    EXPECT_FALSE(fmt.valid());
    EXPECT_EQ(fmt.bytes_per_sample(), 0u);
    EXPECT_EQ(fmt.frame_bytes(), 0u);
}

TEST(AudioFormatTest, ZeroChannelsOrRateRejected)
{
    AudioFormat no_ch { AudioEncoding::PcmS16LE, 0, 48000 };
    EXPECT_FALSE(no_ch.valid());

    AudioFormat no_rate { AudioEncoding::PcmS16LE, 2, 0 };
    EXPECT_FALSE(no_rate.valid());
}

TEST(AudioFormatTest, Equality)
{
    AudioFormat a { AudioEncoding::PcmS16LE, 2, 48000 };
    AudioFormat b { AudioEncoding::PcmS16LE, 2, 48000 };
    AudioFormat c { AudioEncoding::PcmF32LE, 2, 48000 };

    EXPECT_EQ(a, b);
    EXPECT_NE(a, c);
}

TEST(AudioFormatTest, EncodingValuesMatchProto)
{
    // 数值必须与 aqua_service.proto 中 AudioFormat.Encoding 一一对应。
    // 见 AGENT.md §3.4：修改任一端必须同步另一端。
    EXPECT_EQ(static_cast<int>(AudioEncoding::Invalid), 0);
    EXPECT_EQ(static_cast<int>(AudioEncoding::PcmS16LE), 1);
    EXPECT_EQ(static_cast<int>(AudioEncoding::PcmS32LE), 2);
    EXPECT_EQ(static_cast<int>(AudioEncoding::PcmF32LE), 3);
    EXPECT_EQ(static_cast<int>(AudioEncoding::PcmS24LE), 4);
    EXPECT_EQ(static_cast<int>(AudioEncoding::PcmU8), 5);
}
