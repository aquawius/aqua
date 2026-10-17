#include "ui_demo.h"

#include "log_buffer.h"

#include <imgui.h>

#include <cmath>

namespace aqua::gui {

void seed_demo_logs()
{
    LogBuffer& log = log_buffer();
    log.append(kLogInfo, "aqua_client_gui 启动（演示模式：拟真数据，未发起真实连接）");
    log.append(kLogInfo, "输出设备枚举完成，默认跟随系统");
    log.append(kLogInfo, "演示会话 #A3F1：48 kHz / 2ch / F32LE，175 帧每包");
    log.append(kLogDebug, "JitterBuffer: capacity=30 slots, adaptive target on, min_target=7");
    log.append(kLogWarn, "到达抖动 3.2 ms，缓冲水位处于 Warning 带（软校正）");
}

void fill_demo_tiles(double (&v)[6])
{
    const double t = ImGui::GetTime();
    v[0] = 0.42 + 0.05 * std::sin(t * 0.7);
    v[1] = 26.0 + 2.0 * std::sin(t * 0.5);
    v[2] = 26.0 + 2.0 * std::sin(t * 0.5 + 0.4);
    v[3] = 3.2 + 0.6 * std::sin(t * 1.1);
    v[4] = 0.01;
    v[5] = 0.0;
}

} // namespace aqua::gui
