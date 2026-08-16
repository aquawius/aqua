#include "core/public/config.h"

#include <gtest/gtest.h>

// ---- RuntimeConfig 默认值与 config.h 常量一致 ----

TEST(ConfigTest, RuntimeConfigDefaultsMatchConstants)
{
    aqua::config::RuntimeConfig rt;
    EXPECT_EQ(rt.jitter_buffer_ms, aqua::config::DEFAULT_JITTER_BUFFER_MS);
    EXPECT_EQ(rt.playback_ringbuffer_size, aqua::config::DEFAULT_PLAYBACK_RINGBUFFER_BYTES);
    EXPECT_EQ(rt.capture_ringbuffer_size, aqua::config::DEFAULT_CAPTURE_RINGBUFFER_BYTES);
}

TEST(ConfigTest, RuntimeConfigCustomValues)
{
    aqua::config::RuntimeConfig rt;
    rt.jitter_buffer_ms = 120;
    rt.playback_ringbuffer_size = 32 * 1024;
    rt.capture_ringbuffer_size = 16 * 1024;

    EXPECT_EQ(rt.jitter_buffer_ms, 120u);
    EXPECT_EQ(rt.playback_ringbuffer_size, 32768u);
    EXPECT_EQ(rt.capture_ringbuffer_size, 16384u);
}

// ---- JB 单参数分配策略与检测参数一致性 ----

TEST(ConfigTest, DetectWindowParamsConsistent)
{
    // drift rebase 阈值与 AIMD raise 阈值共用同一检测窗口；
    // rebase 阈值应高于 raise 阈值（时间线重建是重操作，触发条件应更严）
    EXPECT_GT(aqua::config::JITTER_DRIFT_REBASE_LATE_COUNT,
        aqua::config::JITTER_DETECT_RAISE_LATE_COUNT);
}

TEST(ConfigTest, JitterBufferCapacityAllocationSane)
{
    // capacity 下限：2 的幂；保证 floor=cap/4 >= 2、ceiling=cap/2 >= 4
    //（自适应区间最小但有效）
    EXPECT_GE(aqua::config::JITTER_MIN_CAPACITY_PACKETS, 8u);
    EXPECT_EQ(aqua::config::JITTER_MIN_CAPACITY_PACKETS & (aqua::config::JITTER_MIN_CAPACITY_PACKETS - 1), 0u);
    // 断流 reset 阈值下限必须大于调度器定时器粒度（Windows ~15.6ms），
    // 否则小 target 时批量唤醒的合法落后被误判为断流（reset 风暴）
    EXPECT_GT(aqua::config::JITTER_MIN_RESET_LATENESS_MS, std::chrono::milliseconds(15));
    EXPECT_LE(aqua::config::JITTER_MIN_RESET_LATENESS_MS, std::chrono::milliseconds(100));
    // 默认容量换算后应覆盖典型 WiFi 抖动（@48kHz 3ms/包 → cap 32 包 = 96ms）
    EXPECT_GE(aqua::config::DEFAULT_JITTER_BUFFER_MS, 30u);
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
