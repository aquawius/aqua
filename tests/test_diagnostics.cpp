#include "core/diagnostics/diagnostics_manager.h"
#include "core/jitter_buffer/jitter_buffer.h"
#include "core/public/audio_format.h"

#include <gtest/gtest.h>

#include <chrono>
#include <thread>

namespace {

aqua::AudioFormat make_test_format() {
    aqua::AudioFormat fmt;
    fmt.encoding = aqua::AudioEncoding::PcmF32LE;
    fmt.channels = 2;
    fmt.sample_rate = 48000;
    return fmt;
}

constexpr std::uint32_t FRAMES_PER_PACKET = 480;
constexpr std::size_t PAYLOAD_SIZE = 480 * 2 * 4;

std::vector<std::byte> make_payload(std::uint32_t sequence) {
    return std::vector<std::byte>(PAYLOAD_SIZE, static_cast<std::byte>(sequence & 0xFF));
}

} // namespace

TEST(DiagnosticsTest, RttMeasurement) {
    std::size_t rb_fill = 0;
    aqua::diag::DiagnosticsManager dm(
        48000, 8, PAYLOAD_SIZE, [&rb_fill]() { return rb_fill; });

    dm.on_hello_sent();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    dm.on_hello_ack_received();

    // 需要 sample_and_log 来更新 snapshot
    aqua::jitter::JitterBuffer jb(make_test_format(), FRAMES_PER_PACKET, 3, 8);
    dm.sample_and_log(jb, std::chrono::seconds(1));

    auto snap = dm.snapshot();
    EXPECT_GT(snap.rtt_ms, 0.0);
    EXPECT_LT(snap.rtt_ms, 100.0);  // 应该在 10ms 左右
}

TEST(DiagnosticsTest, InterarrivalJitter) {
    std::size_t rb_fill = 0;
    aqua::diag::DiagnosticsManager dm(
        48000, 8, PAYLOAD_SIZE, [&rb_fill]() { return rb_fill; });

    // 模拟均匀到达的包（无 jitter）
    for (int i = 0; i < 20; ++i) {
        dm.on_packet_received(static_cast<std::uint32_t>(i),
                              static_cast<std::uint32_t>(i) * 480);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    auto snap = dm.snapshot();
    // 均匀到达时 jitter 应该很小
    EXPECT_LT(snap.interarrival_jitter_ms, 2.0);
}

TEST(DiagnosticsTest, RingBufferOccupancyTracking) {
    std::size_t rb_fill = 0;
    aqua::diag::DiagnosticsManager dm(
        48000, 8, PAYLOAD_SIZE, [&rb_fill]() { return rb_fill; });

    aqua::jitter::JitterBuffer jb(make_test_format(), FRAMES_PER_PACKET, 3, 8);

    // push 3 包到 JitterBuffer
    for (std::uint32_t i = 0; i < 3; ++i) {
        jb.push(i, i * 480, make_payload(i));
    }

    // 模拟 RingBuffer 有数据
    rb_fill = PAYLOAD_SIZE * 5;  // 5 packets worth

    dm.sample_and_log(jb, std::chrono::seconds(1));

    auto snap = dm.snapshot();
    EXPECT_GT(snap.rb_current_ms, 0.0);
    EXPECT_GT(snap.jb_current_ms, 0.0);
}

TEST(DiagnosticsTest, UnderrunCounter) {
    std::size_t rb_fill = 0;
    aqua::diag::DiagnosticsManager dm(
        48000, 8, PAYLOAD_SIZE, [&rb_fill]() { return rb_fill; });

    dm.on_underrun();
    dm.on_underrun();
    dm.on_underrun();

    aqua::jitter::JitterBuffer jb(make_test_format(), FRAMES_PER_PACKET, 3, 8);
    dm.sample_and_log(jb, std::chrono::seconds(1));

    auto snap = dm.snapshot();
    EXPECT_EQ(snap.underruns, 3);
}

TEST(DiagnosticsTest, PacketLossAndLateInSnapshot) {
    std::size_t rb_fill = 0;
    aqua::diag::DiagnosticsManager dm(
        48000, 8, PAYLOAD_SIZE, [&rb_fill]() { return rb_fill; });

    aqua::jitter::JitterBuffer jb(make_test_format(), FRAMES_PER_PACKET, 3, 8);
    std::vector<std::byte> out(PAYLOAD_SIZE);

    // push 100, 101, 103（缺 102）
    jb.push(100, 0, make_payload(100));
    jb.push(101, 480, make_payload(101));
    jb.push(103, 1440, make_payload(103));

    // pop 100, 101, silence(102), 103
    EXPECT_TRUE(jb.pop_next(out));   // 100
    EXPECT_TRUE(jb.pop_next(out));   // 101
    EXPECT_FALSE(jb.pop_next(out));  // 102 = silence (lost)
    EXPECT_TRUE(jb.pop_next(out));   // 103

    // push 102 after deadline → late
    jb.push(102, 960, make_payload(102));

    dm.sample_and_log(jb, std::chrono::seconds(1));

    auto snap = dm.snapshot();
    EXPECT_EQ(snap.packets_lost, 1);
    EXPECT_EQ(snap.late_packets, 1);
    EXPECT_EQ(snap.packets_received, 4);
}

TEST(DiagnosticsTest, EmptySnapshot) {
    std::size_t rb_fill = 0;
    aqua::diag::DiagnosticsManager dm(
        48000, 8, PAYLOAD_SIZE, [&rb_fill]() { return rb_fill; });

    aqua::jitter::JitterBuffer jb(make_test_format(), FRAMES_PER_PACKET, 3, 8);
    dm.sample_and_log(jb, std::chrono::seconds(1));

    auto snap = dm.snapshot();
    EXPECT_EQ(snap.packets_received, 0);
    EXPECT_EQ(snap.packets_lost, 0);
    EXPECT_EQ(snap.rtt_ms, 0.0);
    EXPECT_EQ(snap.underruns, 0);
}
