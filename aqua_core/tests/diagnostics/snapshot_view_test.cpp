// snapshot_view 冒烟测试：用真实的快照（值初始化）驱动 ServerSnapshotView /
// ClientSnapshotView 的每一个模块渲染方法，确认它们都产出紧凑的 "k=v k=v ..." 内容，
// 且每个块以预期的领先键开头。这等价于 server_main / client_main 里注册给
// SnapshotLine 的 lambda 所产出的内容（外层 module{...} 由 SnapshotLine 加）。
//
// 读法见 aqua_core/doc/diagnostics.md。

#include "aqua/audio/capture/audio_capture_config.h"
#include "aqua/diagnostics/snapshot_view.h"

#include <gtest/gtest.h>

#include <string>

namespace {

using aqua::audio::AudioCaptureSource;
using aqua::diagnostics::ClientDiagnosticsSnapshot;
using aqua::diagnostics::ClientSnapshotView;
using aqua::diagnostics::ServerDiagnosticsSnapshot;
using aqua::diagnostics::ServerSnapshotView;

// 断言渲染结果非空，且以预期的前缀（领先键）开头。
void ExpectBlock(const std::string& rendered, std::string_view prefix)
{
    ASSERT_FALSE(rendered.empty()) << "render returned empty for prefix " << prefix;
    EXPECT_EQ(rendered.substr(0, prefix.size()), prefix)
        << "rendered block: " << rendered;
}

TEST(SnapshotViewServerTest, AllModuleBlocksRenderCompact)
{
    ServerDiagnosticsSnapshot s { };
    ServerSnapshotView view(AudioCaptureSource::INPUT_DEVICE);

    ExpectBlock(view.render_state(s, 1234), "state=");
    ExpectBlock(view.render_audio(s), "capture=");
    ExpectBlock(view.render_capture(s), "ev=");
    ExpectBlock(view.render_packetizer(s), "blk=");
    ExpectBlock(view.render_queue(s), "depth=");
    ExpectBlock(view.render_dispatcher(s), "enc=");
    // net 块以逐族 "served=" 开头（先说明本实例在服务哪几族），合计速率仍在同一行。
    const auto net_block = view.render_net(s);
    ExpectBlock(net_block, "served=");
    EXPECT_NE(net_block.find("rx="), std::string::npos);
    ExpectBlock(view.render_sessions(s), "act=");
}

TEST(SnapshotViewClientTest, AllModuleBlocksRenderCompact)
{
    ClientDiagnosticsSnapshot s { };
    ClientSnapshotView view;

    ExpectBlock(view.render_state(s), "state=");
    ExpectBlock(view.render_net(s), "rx=");
    ExpectBlock(view.render_jb(s), "water=");
    ExpectBlock(view.render_jc(s), "adaptive=");
    ExpectBlock(view.render_playback(s, "None"), "running=");
    ExpectBlock(view.render_stream(s), "backend=");
}

TEST(SnapshotViewServerTest, NetBlockShowsPerFamilyDetailOnlyWhenBothFamiliesServed)
{
    ServerDiagnosticsSnapshot s { };
    ServerSnapshotView view(AudioCaptureSource::INPUT_DEVICE);

    // 单族：合计就等于那一族，逐族字段不展开（展开只是噪声）。
    s.net.transport.served_v4 = true;
    s.net.transport.v4.rx_packets = 11;
    auto block = view.render_net(s);
    EXPECT_NE(block.find("served=v4 "), std::string::npos);
    EXPECT_EQ(block.find("v4rx="), std::string::npos);

    // 双族：展开逐族 rx/tx —— "哪一族零收包"必须能一眼看见。
    s.net.transport.served_v6 = true;
    s.net.transport.v6.rx_packets = 7;
    block = view.render_net(s);
    EXPECT_NE(block.find("served=v4+v6 "), std::string::npos);
    EXPECT_NE(block.find("v4rx="), std::string::npos);
    EXPECT_NE(block.find("v6rx="), std::string::npos);
}

TEST(SnapshotViewServerTest, StateBlockIncludesUdpPort)
{
    ServerDiagnosticsSnapshot s { };
    ServerSnapshotView view(AudioCaptureSource::OUTPUT_LOOPBACK);
    const std::string rendered = view.render_state(s, 9000);
    // udp 端口应出现在 state 块内。
    EXPECT_NE(rendered.find("udp=9000"), std::string::npos);
}

} // namespace
