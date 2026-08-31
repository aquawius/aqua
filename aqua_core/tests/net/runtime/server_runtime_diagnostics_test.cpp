#include "aqua/runtime/server_runtime.h"

#include <asio.hpp>
#include <gtest/gtest.h>

namespace {

aqua::runtime::ServerRuntimeConfig make_valid_config()
{
    aqua::runtime::ServerRuntimeConfig cfg;
    cfg.format = aqua::audio::AudioFormat { aqua::audio::AudioEncoding::PCM_F32LE, 2, 48000 };
    cfg.frame_count = 144;
    cfg.server_ip = "127.0.0.1";
    cfg.udp_port = 0;
    cfg.rpc_port = 50052;
    cfg.advertised_udp_address = "127.0.0.1";
    cfg.advertised_udp_port = 50052;
    return cfg;
}

} // namespace

// 聚合快照的字段契约：构造后未 start（Created 状态）全部为初始零值
// （capture_ 尚未创建，各分组计数为 0）。
TEST(ServerRuntimeDiagnosticsTest, CreatedStateSnapshotIsZeroed)
{
    asio::io_context io;
    auto runtime = std::make_shared<aqua::runtime::ServerRuntime>(io, make_valid_config());

    const auto snapshot = runtime->take_diagnostics_snapshot();
    EXPECT_EQ(snapshot.state, aqua::runtime::RuntimeState::Created);
    EXPECT_EQ(snapshot.last_audio_error, aqua::audio::AudioError::None);
    EXPECT_FALSE(snapshot.capture_running);

    EXPECT_EQ(snapshot.capture.audio_events, 0U);
    EXPECT_EQ(snapshot.capture.callbacks, 0U);
    EXPECT_EQ(snapshot.packetizer.input_blocks, 0U);
    EXPECT_EQ(snapshot.packetizer.frames_emitted, 0U);
    EXPECT_EQ(snapshot.queue.accepted_frames, 0U);
    EXPECT_EQ(snapshot.queue.depth_slots, 0U);
    EXPECT_EQ(snapshot.dispatcher.frames_encoded, 0U);
    EXPECT_EQ(snapshot.net.transport.rx_packets, 0U);
    EXPECT_EQ(snapshot.net.hello_received, 0U);
    EXPECT_EQ(snapshot.session.active, std::size_t { 0 });
    EXPECT_EQ(snapshot.session.created, 0U);

    // 音频契约在构造阶段已解析（格式来自 cfg.format）。
    EXPECT_EQ(snapshot.audio_format, *make_valid_config().format);
    EXPECT_EQ(snapshot.frame_count, 144U);
}

// 启动失败（通告地址非法）后：快照状态与 state() 一致（Stopped），
// 查询不产生崩溃或异常。
TEST(ServerRuntimeDiagnosticsTest, SnapshotReflectsFailedStartState)
{
    asio::io_context io;
    auto cfg = make_valid_config();
    cfg.advertised_udp_address = "not-an-ip";

    auto runtime = std::make_shared<aqua::runtime::ServerRuntime>(io, cfg);
    EXPECT_FALSE(runtime->start());

    const auto snapshot = runtime->take_diagnostics_snapshot();
    EXPECT_EQ(snapshot.state, aqua::runtime::RuntimeState::Stopped);
    EXPECT_EQ(snapshot.state, runtime->state());
    EXPECT_FALSE(snapshot.capture_running);
}
