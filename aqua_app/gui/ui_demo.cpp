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

} // namespace aqua::gui
