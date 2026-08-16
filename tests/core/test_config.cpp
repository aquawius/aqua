#include "core/public/config.h"

#include <gtest/gtest.h>

// ---- RuntimeConfig 默认值与 config.h 常量一致 ----

TEST(ConfigTest, RuntimeConfigDefaultsMatchConstants)
{
    aqua::config::RuntimeConfig rt;
    EXPECT_EQ(rt.jitter_target_latency_ms, aqua::config::DEFAULT_JITTER_TARGET_LATENCY_MS);
    EXPECT_EQ(rt.jitter_drift_window_size, aqua::config::JITTER_DRIFT_WINDOW_PACKETS);
    EXPECT_EQ(rt.jitter_drift_late_threshold, aqua::config::JITTER_DRIFT_LATE_PACKET_THRESHOLD);
    EXPECT_EQ(rt.playback_ringbuffer_size, aqua::config::DEFAULT_PLAYBACK_RINGBUFFER_BYTES);
    EXPECT_EQ(rt.capture_ringbuffer_size, aqua::config::DEFAULT_CAPTURE_RINGBUFFER_BYTES);
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

TEST(ConfigTest, HelloHandshakeMaxAttemptsValue)
{
    EXPECT_EQ(aqua::config::HELLO_HANDSHAKE_MAX_ATTEMPTS, 6);
}

TEST(ConfigTest, HelloHandshakeRetryTimeoutAlignsWithSessionTimeout)
{
    // HELLO_HANDSHAKE_RETRY_INTERVAL × HELLO_HANDSHAKE_MAX_ATTEMPTS 应 ≈ SESSION_TIMEOUT
    // 确保 client 在 server session 超时前完成重试
    auto total_retry = aqua::config::HELLO_HANDSHAKE_RETRY_INTERVAL * aqua::config::HELLO_HANDSHAKE_MAX_ATTEMPTS;
    EXPECT_LE(total_retry, aqua::config::SESSION_TIMEOUT)
        << "HELLO 重试总时间不应超过 session 超时";
}

TEST(ConfigTest, KeepaliveIntervalWithinSessionTimeout)
{
    // HELLO_KEEPALIVE_INTERVAL 必须 < SESSION_TIMEOUT / 2
    // 确保超时前至少有 2 次保活机会
    EXPECT_LT(aqua::config::HELLO_KEEPALIVE_INTERVAL, aqua::config::SESSION_TIMEOUT / 2);
}

// ---- 音频包参数 ----

TEST(ConfigTest, FramesPerPacketProducesNoFragmentation)
{
    // 144 frames × 8 bytes/frame (F32LE/2ch) = 1152B payload + 15B header = 1167B < 1500 MTU
    // 48kHz: 144 帧 = 3ms；44.1kHz: 144 帧 ≈ 3.27ms；96kHz: 144 帧 = 1.5ms。
    // 用帧数（而非毫秒）定义包大小，packet_duration = frames/sample_rate
    // 精确等于音频内容真实时长，任何采样率都不会产生截断漂移。
    EXPECT_EQ(aqua::config::AUDIO_FRAMES_PER_PACKET, 144u);
}

// ---- RingBuffer 大小合理 ----

TEST(ConfigTest, PlaybackRingBufferLargerThanCapture)
{
    // 播放缓冲应 >= 采集缓冲（播放侧有 JB 批量 pop + WASAPI 读取双重压力）
    EXPECT_GE(aqua::config::DEFAULT_PLAYBACK_RINGBUFFER_BYTES, aqua::config::DEFAULT_CAPTURE_RINGBUFFER_BYTES);
}

// ---- UDP 接收缓冲 ----

TEST(ConfigTest, UdpRecvBufCoversMaxDatagram)
{
    // 接收缓冲必须 >= 65536（最大 UDP datagram）
    EXPECT_GE(aqua::config::UDP_RECV_BUFFER_BYTES, 65536u);
}

// ---- 诊断刷新间隔 ----

TEST(ConfigTest, DiagnosticsRefreshIntervalSane)
{
    // 下限：过于频繁会让 debug 日志刷屏（每条 diag 行 ~200 字节）。
    // 上限：快照新鲜度劣化，UI/CLI 长时间看不到更新。
    EXPECT_GE(aqua::config::DIAGNOSTICS_REFRESH_INTERVAL, std::chrono::milliseconds(500));
    EXPECT_LE(aqua::config::DIAGNOSTICS_REFRESH_INTERVAL, std::chrono::seconds(60));
}
