#include "ui_main.h"

#include "ui_demo.h"
#include "ui_layout.h"
#include "ui_panels.h"
#include "ui_state.h"

#include <aqua_app/cli/cli_version.h>

#include <imgui.h>

#include <cstdio>

namespace aqua::gui {
namespace {

const char* phase_text(UiPhase phase)
{
    switch (phase) {
    case UiPhase::Connecting:    return "连接中…";
    case UiPhase::Connected:     return "已连接";
    case UiPhase::Disconnecting: return "断开中…";
    case UiPhase::Idle:
    default:                     return "未连接";
    }
}

// ---- 主窗口编排的 UI 线程私有状态 ----
// 与各面板的 *PanelState 同一性质：派生 / 缓存，不属于 UiState。
struct MainWindowState {
    bool demo_seeded = false;      // 演示日志只 seed 一次（进程生命周期）
    float conn_natural_h = 435.0f; // 连接半区自然高度实测值（限高判断用，首帧估值）
};

MainWindowState& main_window_state()
{
    static MainWindowState state;
    return state;
}

// ---- 底部状态栏（重要信息常驻：状态 / 数据源 / 主题与版本）----
// 属于"窗口 chrome"（与主窗口定位、内容 child 一起由本文件编排），故不放
// ui_panels.h 契约里。
void draw_status_bar(const UiState& s)
{
    // 状态点：复用原生色（CheckMark = 绿），不写死色值，三个主题自适应。
    ImVec4 dot_col = ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
    if (s.phase == UiPhase::Connected) {
        dot_col = ImGui::GetStyleColorVec4(ImGuiCol_CheckMark);
    } else if (s.phase == UiPhase::Connecting || s.phase == UiPhase::Disconnecting) {
        dot_col = ImGui::GetStyleColorVec4(ImGuiCol_Text);
    }
    ImGui::TextColored(dot_col, "● %s", phase_text(s.phase));

    ImGui::SameLine();
    if (s.phase == UiPhase::Connected) {
        ImGui::TextDisabled("· 数据源 %s:%s", s.rpc_ip, s.rpc_port);
    } else {
        ImGui::TextDisabled("· 数据源 --");
    }

    // 右侧：主题 · （演示）· 版本（与左侧同一行右对齐）
    char right[96];
    if (s.demo) {
        std::snprintf(right, sizeof(right), "%s · 演示模式 · Aqua Client v%s",
            theme_name(s.theme), AQUA_CLIENT_CLI_VERSION);
    } else {
        std::snprintf(right, sizeof(right), "%s · Aqua Client v%s",
            theme_name(s.theme), AQUA_CLIENT_CLI_VERSION);
    }
    same_line_right(ImGui::CalcTextSize(right).x);
    ImGui::TextDisabled("%s", right);
}

// ---- 主窗口定位 ----
// 主窗口贴在菜单栏下缘、撑满剩余工作区。关键根因：imgui 的主视口工作区
// **已经被自动扣掉主菜单栏**——BeginMainMenuBar → BeginViewportSideBar 把栏高
// 累加进 viewport->BuildWorkInsetMin（imgui_widgets.cpp:9314），次帧
// UpdateViewportsNewFrame 才并进 WorkPos/WorkSize（imgui.cpp:16133）。所以
// 直接贴 WorkPos/WorkSize 就是"紧贴菜单栏下缘"，不需要再自己加菜单栏高度。
// 曾经的 bug：无条件 vp->WorkPos.y + GetFrameHeight()——从第 1 帧起菜单栏被扣
// 两次，主窗顶比菜单栏底缘低一整个 FrameHeight（26px 全黑空隙，用户截图实测
// "菜单栏和 tab 之间有一个空行"），窗口还顺带矮 26px。首帧扣除尚未生效
// （WorkPos.y 还是 0），取 max 兼容两种状态。
void setup_main_window_rect()
{
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float menu_bar_h = ImGui::GetFrameHeight();
    const float top = (vp->WorkPos.y > menu_bar_h) ? vp->WorkPos.y : menu_bar_h;
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x, top));
    ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x, vp->WorkPos.y + vp->WorkSize.y - top));
}

// ---- 上下分屏的"半边"：单项 tab bar + 内容 ----
// 视觉语法（两个半区一致）：tab 坐在下面盒子的顶边框上（folder tab 观感）。
//  imgui 对唯一 tab 会自动选中，内容每帧稳定可见（探针实测连续 4 帧
//  visible=1），不需要 SetSelected，也不会出现当年"每帧 SetSelected 把用户
//  选择拽回旧 tab"的 bug。
//  tab 与内容贴贴（用户 2026-10-16 截图反馈"tab 和下面的框感觉是分开的"）：
//  BeginTabItem 把内容光标落在 BarRect.Max.y + ItemSpacing.y（7px），这里
//  上移一个间距，并把 TabBarBorderSize 压成 0——accent 分隔线由盒子边框接管，
//  不叠成双线。
// 用法（返回 true 后必须调 end_half_tab，push/pop 才能配平）：
//   if (begin_half_tab("##conn_tab", "连接")) { draw_connection_card(s); end_half_tab(); }
bool begin_half_tab(const char* id, const char* label)
{
    ImGui::PushStyleVar(ImGuiStyleVar_TabBarBorderSize, 0.0f);
    if (ImGui::BeginTabBar(id)) {
        if (ImGui::BeginTabItem(label)) {
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() - ImGui::GetStyle().ItemSpacing.y);
            return true;
        }
        ImGui::EndTabBar();
    }
    ImGui::PopStyleVar();
    return false;
}

void end_half_tab()
{
    ImGui::EndTabItem();
    ImGui::EndTabBar();
    ImGui::PopStyleVar();
}

} // namespace

// ---- 主窗口 ----
// 布局（ImHex 式三段）：
//   菜单栏（viewport 级，BeginMainMenuBar）
//   └ 主窗口（从菜单栏下缘起，撑满剩余工作区）
//     ├ 内容 child（底边预留状态栏高度）
//     │  ├ 上半：「连接」单项 tab + 连接卡片（AutoResizeY，占自然高度）
//     │  ├ Separator（上下分屏的分割线）
//     │  └ 下半：「运行日志」单项 tab + 日志面板（吃满剩余高度）
//     ├ Separator
//     └ 状态栏（状态 · 数据源 ……………… 主题 · 版本）
void draw_main_window(UiState& s)
{
    MainWindowState& view = main_window_state();

    // 演示模式首次启动： phase 置 Connected + seed 一批日志（只做一次）。
    if (s.demo && !view.demo_seeded) {
        view.demo_seeded = true;
        s.phase = UiPhase::Connected;
        s.connected_at = ImGui::GetTime();
        seed_demo_logs();
    }

    // 菜单栏贴在 viewport 顶（独立窗口）；主窗口从它下缘开始，避免重叠。
    draw_menu_bar(s);

    setup_main_window_rect();
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize
        | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar
        | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;
    // 菜单栏与主窗口之间不留空行：Begin 会把 style.WindowPadding 锁进
    // window->WindowPadding（imgui.cpp:7769），所以只对 Begin 这一下压成
    // (padding.x, 0) 再立即弹栈——顶 / 底都贴边；卡片与日志面板等子窗口
    // 在弹栈之后才创建，padding 不受影响。
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
        ImVec2(ImGui::GetStyle().WindowPadding.x, 0.0f));
    ImGui::Begin("##aqua_main", nullptr, flags);
    ImGui::PopStyleVar();

    // 内容区 = 除状态栏外的全部高度；负尺寸 = 距底边预留 chrome_h。
    // chrome_h 多留一行文本高度，容纳 Separator 与行距，防止状态栏被挤出底缘。
    const float chrome_h = ImGui::GetFrameHeight() + ImGui::GetTextLineHeightWithSpacing();
    ImGui::BeginChild("##content", ImVec2(0.0f, -chrome_h), ImGuiChildFlags_None);

    // 总可用高度要在加顶部间距之前取（间距之后 GetContentRegionAvail 会少算
    // 一段），下面的限高判断与 conn_natural_h（含间距的实测值）才同基准。
    const float content_avail_h = ImGui::GetContentRegionAvail().y;

    // 菜单栏与 tab 条之间留一个标准块间距（用户 2026-10-16 反馈："连接和上面
    // 的菜单栏中间没有间隔"）。主窗口的 WindowPadding.y 仍压成 0（否则底部
    // 状态栏也会被顶离底缘），只给内容区顶部加一个 WindowPadding.y 的偏移——
    // 底部不受影响，状态栏依旧贴底。
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + ImGui::GetStyle().WindowPadding.y);

    // 连接半区的自然高度（顶部间距 12 + tab 26 + 卡片 AutoResizeY）随字号 /
    // 宽度变化，无法预算；用上一帧实测值判断本轮空间是否够（首帧估值，一帧后
    // 准确）。conn_natural_h 与 content_avail_h 同基准（都从内容 child 顶缘量、
    // 都含顶部间距）：
    //   够   → 不限高：卡片占自然高度，剩余全给日志半区，中间不留死空间；
    //   不够 → 连接半区限高到 (avail - 日志半区下限)，超高时内部滚动，
    //           日志半区始终可见（探针实测 620x460 客户区下布局不散、零 error）。
    constexpr float kMinLogHalfH = 180.0f; // 日志 tab(26) + 面板(~145) + 分割线(9)
    const bool cap_conn = (content_avail_h - view.conn_natural_h) < kMinLogHalfH;
    if (cap_conn) {
        float h = content_avail_h - kMinLogHalfH;
        if (h < 160.0f) {
            h = 160.0f; // 极端矮窗口：连接半区也不至于没有
        }
        ImGui::BeginChild("##conn_half", ImVec2(0.0f, h), ImGuiChildFlags_None);
    }
    if (begin_half_tab("##conn_tab", "连接")) {
        draw_connection_card(s);
        end_half_tab();
    }
    if (cap_conn) {
        ImGui::EndChild();
    } else {
        // 实测自然高度要减掉一个 ItemSpacing.y：卡片 child 提交后 ItemSize 会把
        // 光标多推进一个间距，EndTabBar 的 CurrTabsContentsHeight 含这段——不减
        // 的话估值比真实占用高 7px，默认窗口下会误触发限高包裹，卡片底与分割线
        // 之间露出死空间（2026-10-16 实测 17px，用户对这个敏感）。
        view.conn_natural_h = ImGui::GetCursorPosY() - ImGui::GetStyle().ItemSpacing.y;
    }

    // 中间分割线（用户原话："中间一个分割线，上面一个 tab，下面一个 tab"）。
    ImGui::Separator();

    // 下半：运行日志。日志面板是 Borders 盒子（与连接卡片一致的视觉语法），
    // 尺寸 (0,0) = 吃满剩余高度，窗口拉高时日志区自动变高。
    if (begin_half_tab("##log_tab", "运行日志")) {
        draw_log_panel(s);
        end_half_tab();
    }

    ImGui::EndChild();

    ImGui::Separator();
    draw_status_bar(s);

    ImGui::End();

    // 附加窗口（模态）：统一设置框（外观 / 统计 / 高级 三个 tab）+ 关于。
    draw_settings_window(s);
    draw_about_window(s);
}

} // namespace aqua::gui
