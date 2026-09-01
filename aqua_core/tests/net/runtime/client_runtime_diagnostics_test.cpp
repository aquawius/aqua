#include "aqua/runtime/client_runtime.h"

#include <asio.hpp>
#include <gtest/gtest.h>

namespace {

aqua::runtime::ClientRuntimeConfig make_valid_config()
{
    aqua::runtime::ClientRuntimeConfig cfg;
    cfg.server_ip = "127.0.0.1";
    cfg.rpc_port = 50052;
    cfg.client_name = "test-client";
    return cfg;
}

} // namespace

// 聚合快照的字段契约：Created 状态下全部为初始零值（jb_ 尚未创建，
// capacity/used 为 0 而不是配置值）。
TEST(ClientRuntimeDiagnosticsTest, CreatedStateSnapshotIsZeroed)
{
    asio::io_context io;
    aqua::runtime::ClientRuntime runtime(io, make_valid_config());

    const auto snapshot = runtime.take_diagnostics_snapshot();
    EXPECT_EQ(snapshot.state, aqua::runtime::RuntimeState::Created);
    EXPECT_EQ(snapshot.last_audio_error, aqua::audio::AudioError::None);
    EXPECT_FALSE(snapshot.playback_running);

    EXPECT_EQ(snapshot.net.hello_ack_count, 0U);
    EXPECT_EQ(snapshot.net.hello_ack_misses, 0U);
    // 尚未收到任何 ACK：age 为负哨兵（UdpClient 约定）。
    EXPECT_LT(snapshot.net.hello_ack_age_ms, 0);
    EXPECT_FALSE(snapshot.net.hello_failed);
    EXPECT_EQ(snapshot.net.hello_send_attempts, 0U);
    EXPECT_EQ(snapshot.net.hello_ack_miss_events, 0U);
    EXPECT_EQ(snapshot.net.transport.rx_packets, 0U);
    EXPECT_EQ(snapshot.net.transport.tx_packets, 0U);
    EXPECT_EQ(snapshot.net.audio_frames_accepted, 0U);
    EXPECT_EQ(snapshot.net.malformed_datagrams, 0U);

    EXPECT_DOUBLE_EQ(snapshot.jitter_buffer.water_level, 0.0);
    EXPECT_EQ(snapshot.jitter_buffer.used_slots, 0U);
    EXPECT_EQ(snapshot.jitter_buffer.capacity_slots, 0U);
    EXPECT_EQ(snapshot.jitter_buffer.push_accepted, 0U);
    EXPECT_EQ(snapshot.jitter_buffer.pull_frames, 0U);

    EXPECT_EQ(snapshot.playback.pull_calls, 0U);
    EXPECT_EQ(snapshot.playback.pull_frames, 0U);
    EXPECT_EQ(snapshot.playback.pull_silence_frames, 0U);

    // 输出流参数：未 start → backend=None 全零。
    EXPECT_EQ(snapshot.stream.backend, aqua::audio::AudioStreamInfo::Backend::None);
    EXPECT_EQ(snapshot.stream.sample_rate, 0U);
    EXPECT_EQ(snapshot.stream.frames_per_burst, 0U);
    EXPECT_EQ(snapshot.stream.buffer_capacity_frames, 0U);
}

// 启动失败（server 不可达）后：快照状态与 state() 一致（Stopped），
// 查询不产生崩溃或异常。
TEST(ClientRuntimeDiagnosticsTest, SnapshotReflectsFailedStartState)
{
    asio::io_context io;
    auto cfg = make_valid_config();
    cfg.server_ip = "not-an-ip";

    aqua::runtime::ClientRuntime runtime(io, cfg);
    EXPECT_FALSE(runtime.start());

    const auto snapshot = runtime.take_diagnostics_snapshot();
    EXPECT_EQ(snapshot.state, aqua::runtime::RuntimeState::Stopped);
    EXPECT_EQ(snapshot.state, runtime.state());
    EXPECT_FALSE(snapshot.playback_running);
}
