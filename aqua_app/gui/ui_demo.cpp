#include "ui_demo.h"

#include "log_buffer.h"

#include <imgui.h>

#include <cmath>

namespace aqua::gui {

void seed_demo_logs()
{
    // 演示日志刻意写成英文、并模仿 core 的行文——面板里显示的是"与终端一致的
    // spdlog 输出"，中文日志会立刻露出"这是假的"（用户 2026-10-18 指出）。
    LogBuffer& log = log_buffer();
    log.append(kLogInfo, "aqua_client_gui: demo mode, no real connection (metrics are synthetic)");
    log.append(kLogInfo, "audio device enumeration complete, following system default");
    log.append(kLogInfo, "demo session 0x0000A3F1: 48000 Hz / 2ch / F32LE, 175 frames per packet");
    log.append(kLogDebug, "jitter buffer: capacity=30 slots, adaptive target on, min_target=7");
    log.append(kLogWarn, "arrival jitter 3.2 ms exceeds soft band, target raised to 30.0 ms");
    log.append(kLogInfo, "playback started (shared mode), buffer primed");
}

void fill_demo_tiles_at(double t, double (&v)[6])
{
    v[0] = 0.42 + 0.05 * std::sin(t * 0.7);
    v[1] = 26.0 + 2.0 * std::sin(t * 0.5);
    v[2] = 26.0 + 2.0 * std::sin(t * 0.5 + 0.4);
    v[3] = 3.2 + 0.6 * std::sin(t * 1.1);
    v[4] = 0.01;
    v[5] = 0.0;
}

void fill_demo_tiles(double (&v)[6])
{
    fill_demo_tiles_at(ImGui::GetTime(), v);
}

void fill_demo_link(double (&v)[kDemoLinkCount])
{
    // 与真实会话的量级对齐（175 帧/包 @48kHz ≈ 3.6 ms 一包 ≈ 278 包/秒）：
    // 演示模式整屏都是拟真数据，数字之间也必须自洽，否则一眼假。
    const double t = ImGui::GetTime();
    v[0] = 9.0 + std::floor(2.0 * std::sin(t * 0.5)); // 占用槽
    v[1] = 30.0;                                     // 容量槽
    v[2] = 278.0 + 6.0 * std::sin(t * 0.9);           // 到达包速
    v[3] = 12.0 + 3.0 * std::sin(t * 1.7);            // ACK 延迟 ms
    v[4] = 0.0;                                       // 乱序
    v[5] = 0.0;                                       // 迟到
    v[6] = 0.0;                                       // 重锚
    v[7] = 0.0;                                       // 掩盖
    v[8] = 7.0;                                       // 目标下限槽
    v[9] = 20.0;                                      // 目标上限槽
}

DemoFormat demo_format()
{
    return DemoFormat {
        "48 kHz · 立体声 · PCM F32LE · 1536 kbps · 480 帧 ≈ 10.0 ms",
        "PCM F32LE",
        48000,
        2,
        32,
        1536,
        480,
        10.0,
    };
}

} // namespace aqua::gui
