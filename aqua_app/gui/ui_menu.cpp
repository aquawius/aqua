#include "ui_panels.h"

#include "ui_main.h"
#include "ui_state.h"

#include <imgui.h>

namespace aqua::gui {

// ---- 菜单栏（ImHex 式：viewport 级主菜单，功能与卡片控件同源）----
// viewport 级（BeginMainMenuBar）：imgui 会把它从主视口工作区里扣掉，主窗口
// 直接贴 WorkPos 即可（见 ui_main.cpp 的定位注释）。
void draw_menu_bar(UiState& s)
{
    if (!ImGui::BeginMainMenuBar()) {
        return;
    }
    if (ImGui::BeginMenu("文件")) {
        if (ImGui::MenuItem("连接", "Ctrl+K", false, s.phase == UiPhase::Idle)) {
            toggle_connection(s);
        }
        if (ImGui::MenuItem("断开", nullptr, false, s.phase == UiPhase::Connected)) {
            toggle_connection(s);
        }
        ImGui::Separator();
        if (ImGui::MenuItem("退出")) {
            s.quit_requested = true;
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("视图")) {
        // 上下分屏后连接 / 运行日志都常驻可见，原来的"连接页 / 运行日志"切页
        // 菜单项与 Ctrl+L 一并取消（没有"当前页"可切了）。
        if (ImGui::MenuItem("诊断面板", nullptr, s.show_diagnostics)) {
            s.show_diagnostics = !s.show_diagnostics;
        }
        if (ImGui::MenuItem("高级设置", nullptr, s.show_advanced)) {
            s.show_advanced = !s.show_advanced;
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("设置")) {
        if (ImGui::MenuItem("外观设置…", "Ctrl+P")) {
            s.show_settings = true;
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("帮助")) {
        if (ImGui::MenuItem("关于 Aqua Client")) {
            s.show_about = true;
        }
        ImGui::EndMenu();
    }
    ImGui::EndMainMenuBar();
}

} // namespace aqua::gui
