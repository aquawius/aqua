// diag_view 冒烟测试：用真实的快照聚合（值初始化）驱动 ServerDiagView /
// ClientDiagView 的每一个模块渲染方法，确认它们都产出紧凑的 "k=v k=v ..." 内部
// 内容，且每个模块块以预期的领先键开头。这等价于 server_main / client_main 里
// 注册给 Diagnostics 的 lambda 所产出的内容（外层 module{...} 由 Diagnostics 加）。
//
// 读法见 doc/diagnostics.md。

#include "aqua/audio/capture/audio_capture_config.h"
#include "aqua/diagnostics/diag_view.h"

#include <gtest/gtest.h>

#include <string>

namespace {

using aqua::audio::AudioCaptureSource;
using aqua::diagnostics::ClientDiagnosticsSnapshot;
using aqua::diagnostics::ClientDiagView;
using aqua::diagnostics::ServerDiagnosticsSnapshot;
using aqua::diagnostics::ServerDiagView;

// 断言渲染结果非空，且以预期的前缀（领先键）开头。
void ExpectBlock(const std::string& rendered, std::string_view prefix)
{
    ASSERT_FALSE(rendered.empty()) << "render returned empty for prefix " << prefix;
    EXPECT_EQ(rendered.substr(0, prefix.size()), prefix)
        << "rendered block: " << rendered;
}

TEST(DiagViewServerTest, AllModuleBlocksRenderCompact)
{
    ServerDiagnosticsSnapshot s { };
    ServerDiagView view(AudioCaptureSource::INPUT_DEVICE);

    ExpectBlock(view.render_state(s, 1234), "state=");
    ExpectBlock(view.render_audio(s), "capture=");
    ExpectBlock(view.render_capture(s), "ev=");
    ExpectBlock(view.render_packetizer(s), "blk=");
    ExpectBlock(view.render_queue(s), "depth=");
    ExpectBlock(view.render_dispatcher(s), "enc=");
    ExpectBlock(view.render_net(s), "rx=");
    ExpectBlock(view.render_sessions(s), "act=");
}

TEST(DiagViewClientTest, AllModuleBlocksRenderCompact)
{
    ClientDiagnosticsSnapshot s { };
    ClientDiagView view;

    ExpectBlock(view.render_state(s), "state=");
    ExpectBlock(view.render_net(s), "rx=");
    ExpectBlock(view.render_jb(s), "water=");
    ExpectBlock(view.render_jc(s), "adaptive=");
    ExpectBlock(view.render_playback(s, "None"), "running=");
    ExpectBlock(view.render_stream(s), "backend=");
}

TEST(DiagViewServerTest, StateBlockIncludesUdpPort)
{
    ServerDiagnosticsSnapshot s { };
    ServerDiagView view(AudioCaptureSource::OUTPUT_LOOPBACK);
    const std::string rendered = view.render_state(s, 9000);
    // udp 端口应出现在 state 块内。
    EXPECT_NE(rendered.find("udp=9000"), std::string::npos);
}

} // namespace
