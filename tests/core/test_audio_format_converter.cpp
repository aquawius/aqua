// audio_format_converter 严格测试
// 验证 proto AudioFormat <-> 原生 AudioFormat 的双向转换正确性。
// 覆盖所有编码、字段保留、无效值、极值、往返一致性。

#include <gtest/gtest.h>

#include "core/grpc/audio_format_converter.h"
#include "core/public/audio_format.h"

#include <aqua_service.pb.h>

using aqua::AudioEncoding;
using aqua::AudioFormat;
using aqua::from_proto;
using aqua::to_proto;
// 注意: 不使用 namespace pb = aqua::pb; 别名, 因为 EXPECT_EQ 宏展开时
// protobuf 内部的 pb 符号会遮蔽别名。直接用 aqua::pb:: 引用。

// ---- 所有合法编码的往返转换 ----

TEST(AudioFormatConverterTest, RoundTripAllEncodings)
{
    const struct {
        AudioEncoding enc;
        aqua::pb::AudioFormat::Encoding proto_enc;
    } cases[] = {
        {AudioEncoding::PcmS16LE, aqua::pb::AudioFormat::ENCODING_PCM_S16LE},
        {AudioEncoding::PcmS32LE, aqua::pb::AudioFormat::ENCODING_PCM_S32LE},
        {AudioEncoding::PcmF32LE, aqua::pb::AudioFormat::ENCODING_PCM_F32LE},
        {AudioEncoding::PcmS24LE, aqua::pb::AudioFormat::ENCODING_PCM_S24LE},
        {AudioEncoding::PcmU8,    aqua::pb::AudioFormat::ENCODING_PCM_U8},
    };

    for (const auto& c : cases) {
        AudioFormat src{c.enc, 2, 48000};
        auto proto = to_proto(src);
        EXPECT_EQ(proto.encoding(), c.proto_enc);
        EXPECT_EQ(proto.channels(), 2u);
        EXPECT_EQ(proto.sample_rate(), 48000u);

        auto dst = from_proto(proto);
        EXPECT_EQ(dst.encoding, c.enc);
        EXPECT_EQ(dst.channels, 2u);
        EXPECT_EQ(dst.sample_rate, 48000u);
        EXPECT_TRUE(dst.valid());
        EXPECT_EQ(src, dst);
    }
}

// ---- Invalid 编码往返 ----

TEST(AudioFormatConverterTest, RoundTripInvalidEncoding)
{
    AudioFormat src{AudioEncoding::Invalid, 2, 48000};
    auto proto = to_proto(src);
    EXPECT_EQ(proto.encoding(), aqua::pb::AudioFormat::ENCODING_INVALID);

    auto dst = from_proto(proto);
    EXPECT_EQ(dst.encoding, AudioEncoding::Invalid);
    EXPECT_FALSE(dst.valid());
    EXPECT_EQ(src, dst);
}

// ---- 默认构造的 AudioFormat (Invalid, 0, 0) ----

TEST(AudioFormatConverterTest, DefaultConstructedRoundTrip)
{
    AudioFormat src{};
    auto proto = to_proto(src);
    EXPECT_EQ(proto.encoding(), aqua::pb::AudioFormat::ENCODING_INVALID);
    EXPECT_EQ(proto.channels(), 0u);
    EXPECT_EQ(proto.sample_rate(), 0u);

    auto dst = from_proto(proto);
    EXPECT_EQ(src, dst);
    EXPECT_FALSE(dst.valid());
}

// ---- 字段保留: 极值 ----

TEST(AudioFormatConverterTest, ExtremeFieldValuesPreserved)
{
    AudioFormat src{AudioEncoding::PcmS32LE, UINT32_MAX, UINT32_MAX};
    auto proto = to_proto(src);
    auto dst = from_proto(proto);
    EXPECT_EQ(dst.channels, UINT32_MAX);
    EXPECT_EQ(dst.sample_rate, UINT32_MAX);
    EXPECT_EQ(dst.encoding, AudioEncoding::PcmS32LE);
    EXPECT_EQ(src, dst);
}

// ---- 单声道 / 立体声 / 7.1 声道 ----

TEST(AudioFormatConverterTest, VariousChannelCounts)
{
    for (uint32_t ch : {1u, 2u, 6u, 8u}) {
        AudioFormat src{AudioEncoding::PcmS16LE, ch, 44100};
        auto dst = from_proto(to_proto(src));
        EXPECT_EQ(dst.channels, ch);
        EXPECT_EQ(src, dst);
    }
}

// ---- 常见采样率 ----

TEST(AudioFormatConverterTest, CommonSampleRates)
{
    for (uint32_t sr : {8000u, 16000u, 22050u, 32000u, 44100u, 48000u, 96000u, 192000u}) {
        AudioFormat src{AudioEncoding::PcmF32LE, 2, sr};
        auto dst = from_proto(to_proto(src));
        EXPECT_EQ(dst.sample_rate, sr);
        EXPECT_EQ(src, dst);
    }
}

// ---- bytes_per_sample 与编码一致性 (转换后仍能正确计算) ----

TEST(AudioFormatConverterTest, BytesPerSampleAfterConversion)
{
    const struct { AudioEncoding enc; uint32_t bps; } cases[] = {
        {AudioEncoding::PcmU8,    1u},
        {AudioEncoding::PcmS16LE, 2u},
        {AudioEncoding::PcmS24LE, 3u},
        {AudioEncoding::PcmS32LE, 4u},
        {AudioEncoding::PcmF32LE, 4u},
    };

    for (const auto& c : cases) {
        AudioFormat src{c.enc, 2, 48000};
        auto dst = from_proto(to_proto(src));
        EXPECT_EQ(dst.bytes_per_sample(), c.bps);
        EXPECT_EQ(dst.frame_bytes(), c.bps * 2u);
    }
}

// ---- 未知 proto encoding 值映射为 Invalid ----

TEST(AudioFormatConverterTest, UnknownProtoEncodingMapsToInvalid)
{
    aqua::pb::AudioFormat proto;
    proto.set_encoding(static_cast<aqua::pb::AudioFormat::Encoding>(999));
    proto.set_channels(2);
    proto.set_sample_rate(48000);

    auto dst = from_proto(proto);
    EXPECT_EQ(dst.encoding, AudioEncoding::Invalid);
    EXPECT_FALSE(dst.valid());
    // channels/sample_rate 仍被保留
    EXPECT_EQ(dst.channels, 2u);
    EXPECT_EQ(dst.sample_rate, 48000u);
}

// ---- 默认 proto (encoding=0=INVALID) ----

TEST(AudioFormatConverterTest, DefaultProtoMapsToInvalid)
{
    aqua::pb::AudioFormat proto;  // 默认 encoding=0=ENCODING_INVALID
    auto dst = from_proto(proto);
    EXPECT_EQ(dst.encoding, AudioEncoding::Invalid);
    EXPECT_EQ(dst.channels, 0u);
    EXPECT_EQ(dst.sample_rate, 0u);
    EXPECT_FALSE(dst.valid());
}

// ---- 数值一致性: 与 proto 枚举数值严格对应 ----

TEST(AudioFormatConverterTest, EnumValuesStrictlyMatchProto)
{
    // 修改任一端必须同步另一端 (见 AGENT.md §3.4)
    AudioFormat fmt_s16{AudioEncoding::PcmS16LE, 1, 8000};
    EXPECT_EQ(to_proto(fmt_s16).encoding(), aqua::pb::AudioFormat::ENCODING_PCM_S16LE);
    EXPECT_EQ(static_cast<int>(to_proto(fmt_s16).encoding()), 1);

    AudioFormat fmt_s32{AudioEncoding::PcmS32LE, 1, 8000};
    EXPECT_EQ(to_proto(fmt_s32).encoding(), aqua::pb::AudioFormat::ENCODING_PCM_S32LE);
    EXPECT_EQ(static_cast<int>(to_proto(fmt_s32).encoding()), 2);

    AudioFormat fmt_f32{AudioEncoding::PcmF32LE, 1, 8000};
    EXPECT_EQ(to_proto(fmt_f32).encoding(), aqua::pb::AudioFormat::ENCODING_PCM_F32LE);
    EXPECT_EQ(static_cast<int>(to_proto(fmt_f32).encoding()), 3);

    AudioFormat fmt_s24{AudioEncoding::PcmS24LE, 1, 8000};
    EXPECT_EQ(to_proto(fmt_s24).encoding(), aqua::pb::AudioFormat::ENCODING_PCM_S24LE);
    EXPECT_EQ(static_cast<int>(to_proto(fmt_s24).encoding()), 4);

    AudioFormat fmt_u8{AudioEncoding::PcmU8, 1, 8000};
    EXPECT_EQ(to_proto(fmt_u8).encoding(), aqua::pb::AudioFormat::ENCODING_PCM_U8);
    EXPECT_EQ(static_cast<int>(to_proto(fmt_u8).encoding()), 5);
}

// ---- 多次往返转换稳定性 ----

TEST(AudioFormatConverterTest, MultipleRoundTripsStable)
{
    AudioFormat fmt{AudioEncoding::PcmS24LE, 6, 96000};
    for (int i = 0; i < 10; ++i) {
        fmt = from_proto(to_proto(fmt));
    }
    EXPECT_EQ(fmt.encoding, AudioEncoding::PcmS24LE);
    EXPECT_EQ(fmt.channels, 6u);
    EXPECT_EQ(fmt.sample_rate, 96000u);
    EXPECT_TRUE(fmt.valid());
}
