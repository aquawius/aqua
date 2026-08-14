#include "core/public/config.h"

#include <gtest/gtest.h>

// ---- RuntimeConfig 默认值与 config.h 常量一致 ----

TEST(ConfigTest, RuntimeConfigDefaultsMatchConstants)
{
    aqua::config::RuntimeConfig rt;
    EXPECT_EQ(rt.jitter_target_latency_ms, aqua::config::JITTER_TARGET_LATENCY_MS);
    EXPECT_EQ(rt.jitter_drift_window_size, aqua::config::JITTER_DRIFT_WINDOW_SIZE);
    EXPECT_EQ(rt.jitter_drift_late_threshold, aqua::config::JITTER_DRIFT_LATE_THRESHOLD);
    EXPECT_EQ(rt.playback_ringbuffer_size, aqua::config::PLAYBACK_RINGBUFFER_SIZE);
    EXPECT_EQ(rt.capture_ringbuffer_size, aqua::config::CAPTURE_RINGBUFFER_SIZE);
}

TEST(ConfigTest, RuntimeConfigCustomValues)
{
    aqua::config::RuntimeConfig rt;
    rt.jitter_target_latency_ms = 20;
    rt.jitter_drift_window_size = 500;
    rt.jitter_drift_late_threshold = 10;
    rt.playback_ringbuffer_size = 32 * 1024;
    rt.capture_ringbuffer_size = 16 * 1024;

    EXPECT_EQ(rt.jitter_target_latency_ms, 20u);
    EXPECT_EQ(rt.jitter_drift_window_size, 500u);
    EXPECT_EQ(rt.jitter_drift_late_threshold, 10u);
    EXPECT_EQ(rt.playback_ringbuffer_size, 32768u);
    EXPECT_EQ(rt.capture_ringbuffer_size, 16384u);
}

// ---- HELLO 重试与超时对齐 ----

TEST(ConfigTest, HelloMaxRetriesValue)
{
    EXPECT_EQ(aqua::config::HELLO_MAX_RETRIES, 6);
}

TEST(ConfigTest, HelloRetryTimeoutAlignsWithSessionTimeout)
{
    // HELLO_RETRY_INTERVAL × HELLO_MAX_RETRIES 应 ≈ UDP_SESSION_TIMEOUT
    // 确保 client 在 server session 超时前完成重试
    auto total_retry = aqua::config::HELLO_RETRY_INTERVAL * aqua::config::HELLO_MAX_RETRIES;
    EXPECT_LE(total_retry, aqua::config::UDP_SESSION_TIMEOUT)
        << "HELLO 重试总时间不应超过 session 超时";
}

TEST(ConfigTest, KeepaliveIntervalWithinSessionTimeout)
{
    // KEEPALIVE_INTERVAL 必须 < UDP_SESSION_TIMEOUT / 2
    // 确保超时前至少有 2 次保活机会
    EXPECT_LT(aqua::config::KEEPALIVE_INTERVAL, aqua::config::UDP_SESSION_TIMEOUT / 2);
}

// ---- 音频包参数 ----

TEST(ConfigTest, AudioPacketMsProducesNoFragmentation)
{
    // 3ms × 48kHz = 144 frames × 8 bytes = 1152B payload + header < 1500 MTU
    EXPECT_EQ(aqua::config::AUDIO_PACKET_MS, 3u);
}

// ---- RingBuffer 大小合理 ----

TEST(ConfigTest, PlaybackRingBufferLargerThanCapture)
{
    // 播放缓冲应 >= 采集缓冲（播放侧有 JB 批量 pop + WASAPI 读取双重压力）
    EXPECT_GE(aqua::config::PLAYBACK_RINGBUFFER_SIZE, aqua::config::CAPTURE_RINGBUFFER_SIZE);
}

// ---- UDP 接收缓冲 ----

TEST(ConfigTest, UdpRecvBufCoversMaxDatagram)
{
    // 接收缓冲必须 >= 65536（最大 UDP datagram）
    EXPECT_GE(aqua::config::UDP_RECV_BUF_SIZE, 65536u);
}
