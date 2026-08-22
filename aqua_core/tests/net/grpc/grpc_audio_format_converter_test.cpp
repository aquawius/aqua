#include "aqua/audio/audio_format_converter.h"

#include <gtest/gtest.h>

#include <array>
#include <limits>


namespace {

TEST(GrpcAudioFormatConverterTest, ConvertsValidS16Stereo)
{
    aqua::pb::AudioFormat proto;
    proto.set_encoding(aqua::pb::AudioFormat::ENCODING_PCM_S16LE);
    proto.set_channels(2);
    proto.set_sample_rate(48000);

    const auto fmt = aqua::from_proto(proto);
    ASSERT_TRUE(fmt.is_valid());
    EXPECT_EQ(fmt.encoding, aqua::audio::AudioEncoding::PCM_S16LE);
    EXPECT_EQ(fmt.channels, 2u);
    EXPECT_EQ(fmt.sample_rate, 48000u);
}

TEST(GrpcAudioFormatConverterTest, RejectsInvalidEncoding)
{
    aqua::pb::AudioFormat proto;
    proto.set_encoding(aqua::pb::AudioFormat::ENCODING_INVALID);
    proto.set_channels(2);
    proto.set_sample_rate(48000);

    EXPECT_FALSE(aqua::from_proto(proto).is_valid());
}

TEST(GrpcAudioFormatConverterTest, RejectsInvalidChannels)
{
    aqua::pb::AudioFormat proto;
    proto.set_encoding(aqua::pb::AudioFormat::ENCODING_PCM_F32LE);
    proto.set_channels(0);
    proto.set_sample_rate(48000);
    EXPECT_FALSE(aqua::from_proto(proto).is_valid());

    proto.set_channels(-1);
    EXPECT_FALSE(aqua::from_proto(proto).is_valid());
}

TEST(GrpcAudioFormatConverterTest, RejectsInvalidSampleRate)
{
    aqua::pb::AudioFormat proto;
    proto.set_encoding(aqua::pb::AudioFormat::ENCODING_PCM_F32LE);
    proto.set_channels(2);
    proto.set_sample_rate(0);
    EXPECT_FALSE(aqua::from_proto(proto).is_valid());

    proto.set_sample_rate(-1);
    EXPECT_FALSE(aqua::from_proto(proto).is_valid());
}

TEST(GrpcAudioFormatConverterTest, ConvertsAllSupportedEncodings)
{
    const std::array encodings {
        aqua::pb::AudioFormat::ENCODING_PCM_S16LE,
        aqua::pb::AudioFormat::ENCODING_PCM_S32LE,
        aqua::pb::AudioFormat::ENCODING_PCM_F32LE,
        aqua::pb::AudioFormat::ENCODING_PCM_S24LE,
        aqua::pb::AudioFormat::ENCODING_PCM_U8,
    };

    for (const auto encoding : encodings) {
        aqua::pb::AudioFormat proto;
        proto.set_encoding(encoding);
        proto.set_channels(2);
        proto.set_sample_rate(48000);
        EXPECT_TRUE(aqua::from_proto(proto).is_valid());
    }
}

TEST(GrpcAudioFormatConverterTest, NativeToProtoRoundTrips)
{
    aqua::audio::AudioFormat source;
    source.encoding = aqua::audio::AudioEncoding::PCM_F32LE;
    source.channels = 2;
    source.sample_rate = 48000;

    const auto proto = aqua::to_proto(source);
    const auto round_trip = aqua::from_proto(proto);

    EXPECT_EQ(round_trip.encoding, source.encoding);
    EXPECT_EQ(round_trip.channels, source.channels);
    EXPECT_EQ(round_trip.sample_rate, source.sample_rate);
}

TEST(GrpcAudioFormatConverterTest, NativeInvalidEncodingMapsToProtoInvalid)
{
    aqua::audio::AudioFormat source;
    source.encoding = aqua::audio::AudioEncoding::INVALID;
    source.channels = 0;
    source.sample_rate = 0;

    const auto proto = aqua::to_proto(source);
    EXPECT_EQ(proto.encoding(), aqua::pb::AudioFormat::ENCODING_INVALID);
}

TEST(GrpcAudioFormatConverterTest, RejectsOverflowingValues)
{
    // proto3 int32 无范围约束：INT32_MAX 经隐式转换会变成巨大 uint32，
    // 必须在转换层拦截（见 from_proto 的显式校验）。
    aqua::pb::AudioFormat proto;
    proto.set_encoding(aqua::pb::AudioFormat::ENCODING_PCM_S16LE);
    proto.set_channels(std::numeric_limits<std::int32_t>::max());
    proto.set_sample_rate(48000);
    EXPECT_FALSE(aqua::from_proto(proto).is_valid());

    proto.set_channels(2);
    proto.set_sample_rate(std::numeric_limits<std::int32_t>::max());
    EXPECT_FALSE(aqua::from_proto(proto).is_valid());
}

TEST(GrpcAudioFormatConverterTest, RejectsUnknownEncodingValue)
{
    // 未来新增编码枚举前的未知值：default 分支归一到 INVALID。
    aqua::pb::AudioFormat proto;
    proto.set_encoding(static_cast<aqua::pb::AudioFormat::Encoding>(999));
    proto.set_channels(2);
    proto.set_sample_rate(48000);
    EXPECT_FALSE(aqua::from_proto(proto).is_valid());
}

} // namespace
