#include "core/diagnostics/diagnostics_manager.h"
#include "core/jitter_buffer/jitter_buffer.h"
#include "core/public/audio_format.h"

#include <gtest/gtest.h>

#include <chrono>
#include <thread>

namespace {

aqua::AudioFormat make_test_format()
{
    aqua::AudioFormat fmt;
    fmt.encoding = aqua::AudioEncoding::PcmF32LE;
    fmt.channels = 2;
    fmt.sample_rate = 48000;
    return fmt;
}

constexpr std::uint32_t FRAMES_PER_PACKET = 480;
constexpr std::size_t PAYLOAD_SIZE = 480 * 2 * 4;

std::vector<std::byte> make_payload(std::uint32_t sequence)
{
    return std::vector<std::byte>(PAYLOAD_SIZE, static_cast<std::byte>(sequence & 0xFF));
}

} // namespace

TEST(DiagnosticsTest, RttMeasurement)
{
    std::size_t rb_fill = 0;
    aqua::diag::DiagnosticsManager dm(
        48000, 8, PAYLOAD_SIZE, [&rb_fill]() { return rb_fill; }, PAYLOAD_SIZE * 8);

    dm.record_hello_sent();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    dm.record_hello_ack_received();

    // 需要 collect_and_log 来更新 snapshot
    aqua::jitter::JitterBuffer jb(make_test_format(), FRAMES_PER_PACKET, 3, 8);
    dm.collect_and_log(jb);

    auto snap = dm.snapshot();
    EXPECT_GT(snap.rtt_ms, 0.0);
    EXPECT_LT(snap.rtt_ms, 100.0); // 应该在 10ms 左右
}

TEST(DiagnosticsTest, InterarrivalJitter)
{
    std::size_t rb_fill = 0;
    aqua::diag::DiagnosticsManager dm(
        48000, 8, PAYLOAD_SIZE, [&rb_fill]() { return rb_fill; }, PAYLOAD_SIZE * 8);

    // 模拟均匀到达的包（无 jitter）
    for (int i = 0; i < 20; ++i) {
        dm.record_packet_arrival(static_cast<std::uint32_t>(i),
            static_cast<std::uint32_t>(i) * 480);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    auto snap = dm.snapshot();
    // 均匀到达时 jitter 应该很小
    EXPECT_LT(snap.interarrival_jitter_ms, 2.0);
}

TEST(DiagnosticsTest, RingBufferOccupancyTracking)
{
    std::size_t rb_fill = 0;
    aqua::diag::DiagnosticsManager dm(
        48000, 8, PAYLOAD_SIZE, [&rb_fill]() { return rb_fill; }, PAYLOAD_SIZE * 8);

    aqua::jitter::JitterBuffer jb(make_test_format(), FRAMES_PER_PACKET, 3, 8);

    // push 3 包到 JitterBuffer
    for (std::uint32_t i = 0; i < 3; ++i) {
        jb.push(i, make_payload(i));
    }

    // 模拟 RingBuffer 有数据
    rb_fill = PAYLOAD_SIZE * 5; // 5 packets worth

    dm.collect_and_log(jb);

    auto snap = dm.snapshot();
    EXPECT_GT(snap.rb_current_ms, 0.0);
    EXPECT_GT(snap.jb_current_ms, 0.0);
}

TEST(DiagnosticsTest, UnderrunCounter)
{
    std::size_t rb_fill = 0;
    aqua::diag::DiagnosticsManager dm(
        48000, 8, PAYLOAD_SIZE, [&rb_fill]() { return rb_fill; }, PAYLOAD_SIZE * 8);

    dm.record_underrun();
    dm.record_underrun();
    dm.record_underrun();

    aqua::jitter::JitterBuffer jb(make_test_format(), FRAMES_PER_PACKET, 3, 8);
    dm.collect_and_log(jb);

    auto snap = dm.snapshot();
    EXPECT_EQ(snap.underruns, 3);
}

TEST(DiagnosticsTest, PacketLossAndLateInSnapshot)
{
    std::size_t rb_fill = 0;
    aqua::diag::DiagnosticsManager dm(
        48000, 8, PAYLOAD_SIZE, [&rb_fill]() { return rb_fill; }, PAYLOAD_SIZE * 8);

    aqua::jitter::JitterBuffer jb(make_test_format(), FRAMES_PER_PACKET, 3, 8);
    std::vector<std::byte> out(PAYLOAD_SIZE);

    // push 100, 101, 103（缺 102）
    jb.push(100, make_payload(100));
    jb.push(101, make_payload(101));
    jb.push(103, make_payload(103));

    // pop 100, 101, silence(102), 103
    EXPECT_TRUE(jb.pop_next(out)); // 100
    EXPECT_TRUE(jb.pop_next(out)); // 101
    EXPECT_FALSE(jb.pop_next(out)); // 102 = silence (lost)
    EXPECT_TRUE(jb.pop_next(out)); // 103

    // push 102 after deadline → late
    jb.push(102, make_payload(102));

    dm.collect_and_log(jb);

    auto snap = dm.snapshot();
    EXPECT_EQ(snap.packets_lost, 1);
    EXPECT_EQ(snap.late_packets, 1);
    EXPECT_EQ(snap.packets_received, 4);
}

TEST(DiagnosticsTest, EmptySnapshot)
{
    std::size_t rb_fill = 0;
    aqua::diag::DiagnosticsManager dm(
        48000, 8, PAYLOAD_SIZE, [&rb_fill]() { return rb_fill; }, PAYLOAD_SIZE * 8);

    aqua::jitter::JitterBuffer jb(make_test_format(), FRAMES_PER_PACKET, 3, 8);
    dm.collect_and_log(jb);

    auto snap = dm.snapshot();
    EXPECT_EQ(snap.packets_received, 0);
    EXPECT_EQ(snap.packets_lost, 0);
    EXPECT_EQ(snap.rtt_ms, 0.0);
    EXPECT_EQ(snap.underruns, 0);
}

TEST(DiagnosticsTest, EndToEndLatencyIsBufferedAudio)
{
    std::uint64_t played = 0;
    std::size_t rb_fill = PAYLOAD_SIZE * 5; // 5 包 = 50ms
    aqua::diag::DiagnosticsManager dm(
        48000, 8, PAYLOAD_SIZE,
        [&rb_fill]() { return rb_fill; },
        PAYLOAD_SIZE * 8,
        [&played]() { return played; });

    aqua::jitter::JitterBuffer jb(make_test_format(), FRAMES_PER_PACKET, 3, 8);
    // JB 缓冲 3 包 = 30ms
    jb.push(0, make_payload(0));
    jb.push(1, make_payload(1));
    jb.push(2, make_payload(2));

    dm.collect_and_log(jb);

    auto snap = dm.snapshot();
    // e2e = 当前缓冲量 = JB(30ms) + RB(50ms) = 80ms，无需时间同步
    EXPECT_NEAR(snap.end_to_end_ms, 80.0, 1.0);
}

TEST(DiagnosticsTest, DriftZeroWhenRatesMatch)
{
    std::uint64_t played = 0;
    std::size_t rb_fill = 0;
    aqua::diag::DiagnosticsManager dm(
        48000, 8, PAYLOAD_SIZE,
        [&rb_fill]() { return rb_fill; },
        PAYLOAD_SIZE * 8,
        [&played]() { return played; });

    aqua::jitter::JitterBuffer jb(make_test_format(), FRAMES_PER_PACKET, 3, 8);

    // server 与本地播放都以 480 帧/10ms（=48kHz）推进，漂移应≈0
    for (int i = 0; i < 20; ++i) {
        dm.record_packet_arrival(i, static_cast<std::uint32_t>(i) * 480);
        played += 480;
        dm.record_rb_occupancy();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    dm.collect_and_log(jb);
    auto snap = dm.snapshot();
    EXPECT_NEAR(snap.drift_ppm, 0.0, 2000.0); // 留抖动余量
}

TEST(DiagnosticsTest, DriftPositiveWhenServerFaster)
{
    std::uint64_t played = 0;
    std::size_t rb_fill = 0;
    aqua::diag::DiagnosticsManager dm(
        48000, 8, PAYLOAD_SIZE,
        [&rb_fill]() { return rb_fill; },
        PAYLOAD_SIZE * 8,
        [&played]() { return played; });

    aqua::jitter::JitterBuffer jb(make_test_format(), FRAMES_PER_PACKET, 3, 8);

    // server 480 帧/5ms（=96kHz，快于本地播放 48kHz）→ 漂移应为明显正值。
    // arrival 每 5ms 推进 480 帧；played 每 10ms 推进 480 帧。
    for (int i = 0; i < 40; ++i) {
        dm.record_packet_arrival(i, static_cast<std::uint32_t>(i) * 480);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        if (i % 2 == 0) { // 每 10ms 采样一次播放进度
            played += 480;
            dm.record_rb_occupancy();
        }
    }

    dm.collect_and_log(jb);
    auto snap = dm.snapshot();
    EXPECT_GT(snap.drift_ppm, 50000.0); // server 明显快于播放
}
