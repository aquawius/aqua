#include "ui_panels.h"

#include "ui_main.h"
#include "ui_state.h"

#include <imgui.h>

namespace aqua::gui {

// ---- 菜单栏（viewport 级主菜单，功能与卡片控件同源）----
// viewport 级（BeginMainMenuBar）：imgui 会把它从主视口工作区里扣掉，主窗口
// 直接贴 WorkPos 即可（见 ui_main.cpp 的定位注释）。
// 2026-10-17 按用户要求瘦身：只留 文件 / 设置 / 帮助（视图菜单取消——连接与
// 运行日志都常驻，诊断/高级设置并入统一设置框的 tab）。
// 2026-10-18：菜单项**不标快捷键**（无键盘加速，见 main.cpp 的说明）。
void draw_menu_bar(UiState& s)
{
    if (!ImGui::BeginMainMenuBar()) {
        return;
    }
    if (ImGui::BeginMenu("文件")) {
        const bool can_connect = (s.phase == UiPhase::Idle || s.phase == UiPhase::Failed);
        if (ImGui::MenuItem("连接", nullptr, false, can_connect)) {
            toggle_connection(s);
        }
        // 断开对"已连接 / 连接中 / 重连中"都可用：后两者等于"取消"。
        // 曾经只允许 Connected —— 重连一个不可达的地址时，菜单里也没有出路。
        const bool can_disconnect = (s.phase == UiPhase::Connected
            || s.phase == UiPhase::Connecting || s.phase == UiPhase::Reconnecting);
        if (ImGui::MenuItem("断开", nullptr, false, can_disconnect)) {
            toggle_connection(s);
        }
        ImGui::Separator();
        if (ImGui::MenuItem("退出")) {
            s.quit_requested = true;
        }
        ImGui::EndMenu();
    }
    // 四个入口都打开同一个设置框，只是落在不同 tab（settings_tab 请求位）。
    if (ImGui::BeginMenu("设置")) {
        if (ImGui::MenuItem("外观设置…")) {
            s.settings_tab = kSettingsAppearance;
            s.show_settings = true;
        }
        if (ImGui::MenuItem("连接设置…")) {
            s.settings_tab = kSettingsConnection;
            s.show_settings = true;
        }
        if (ImGui::MenuItem("统计…")) {
            s.settings_tab = kSettingsStats;
            s.show_settings = true;
        }
        if (ImGui::MenuItem("高级设置…")) {
            s.settings_tab = kSettingsAdvanced;
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
