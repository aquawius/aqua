#include "ui_main.h"

#include "ui_demo.h"
#include "ui_layout.h"
#include "ui_panels.h"
#include "ui_state.h"

#include <imgui.h>

#include <cstdio>
#include <string>

namespace aqua::gui {
namespace {

const char* phase_text(UiPhase phase)
{
    switch (phase) {
    case UiPhase::Connecting:    return "连接中…";
    case UiPhase::Connected:     return "已连接";
    case UiPhase::Disconnecting: return "断开中…";
    case UiPhase::Failed:        return "连接失败";
    case UiPhase::Idle:
    default:                     return "未连接";
    }
}

// ---- 主窗口编排的 UI 线程私有状态 ----
struct MainWindowState {
    bool demo_seeded = false; // 演示日志只 seed 一次（进程生命周期）
};

MainWindowState& main_window_state()
{
    static MainWindowState state;
    return state;
}

// ---- 底部状态栏 ----
// 只放"现在正在做什么 / 连在哪里 / 是哪一场会话"这类常驻信息（主题与版本不重要，
// 撤掉；版本在「关于」里）：
//   左：● 已连接 · 控制 127.0.0.1:50051 · 数据 127.0.0.1:50000 · 会话 0xAC3EA304
//   右：运行时长（仅连接后）；演示模式给个标记
// 控制面 / 数据面分开写：两者本来就分别绑定（见 AGENT.md §6），连上之后"发到哪
// 个 UDP 端点"是排障第一问；会话号用来和日志（core 每行都带 session=0x…）对上。
// **不**放包速 / 音频格式：前者在右侧链路卡里，后者是遥测面板的第一行（用户
// 2026-10-18：状态栏不要和面板重复）。
// 左段按"信息量从多到少"排好候选，放不下就降级——窄窗口时先丢会话号，再丢数据面，
// 保证最关键的"连到哪、在做什么"永远看得见。
void draw_status_bar(const UiState& s)
{
    // 状态点：复用原生色（CheckMark = 绿），不写死色值，三个主题自适应。
    ImVec4 dot_col = ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
    if (s.phase == UiPhase::Connected) {
        dot_col = ImGui::GetStyleColorVec4(ImGuiCol_CheckMark);
    } else if (s.phase == UiPhase::Connecting || s.phase == UiPhase::Disconnecting
        || s.phase == UiPhase::Failed) {
        dot_col = ImGui::GetStyleColorVec4(ImGuiCol_Text);
    }

    const ImGuiStyle& st = ImGui::GetStyle();

    // 右段先定：运行时长（连接后）/ 演示模式标记。它的宽度决定左段能用多少。
    // 演示模式给标记而不是跑秒——那里的"会话"是拟真的，跑秒没有意义。
    std::string right;
    if (s.demo) {
        right = "演示模式";
    } else if (s.phase == UiPhase::Connected) {
        right = format_duration(ImGui::GetTime() - s.connected_at);
    }
    const float right_w = right.empty() ? 0.0f : ImGui::CalcTextSize(right.c_str()).x;
    const float gap = st.ItemSpacing.x * 2.0f;

    // 左段：阶段 + 正在做的事。非 Connected 阶段直接采用会话层的最新说明——"正在
    // 连接 …" / "连接失败：…" / "会话中断，3 秒后自动重连（第 1 次）"本身已经
    // 说明在做什么，再拼一遍 phase_text 就成了"连接失败 · 连接失败: …"。
    std::string left;
    if (s.phase == UiPhase::Connected) {
        const std::string control = "已连接 · 控制 " + std::string(s.rpc_ip) + ":" + s.rpc_port;
        std::string full = control;
        if (!s.session.data_plane.empty()) {
            full += " · 数据 " + s.session.data_plane;
        }
        std::string with_session = full;
        if (s.session.session_id != 0) {
            char sid[24];
            std::snprintf(sid, sizeof(sid), " · 会话 0x%08X",
                static_cast<unsigned>(s.session.session_id));
            with_session += sid;
        }
        // 可用宽度：内容右缘 - 当前行起点 - 右段 - 间隔。
        const float avail = ImGui::GetContentRegionMax().x - ImGui::GetCursorPosX()
            - right_w - gap;
        // "● " 前缀也占宽，一起算进来。
        const float dot_w = ImGui::CalcTextSize("● ").x;
        if (ImGui::CalcTextSize(with_session.c_str()).x <= avail - dot_w) {
            left = with_session;
        } else if (ImGui::CalcTextSize(full.c_str()).x <= avail - dot_w) {
            left = full;
        } else {
            left = control;
        }
    } else if (s.phase != UiPhase::Idle && !s.session.status_detail.empty()) {
        left = s.session.status_detail;
    } else {
        left = phase_text(s.phase);
    }
    ImGui::TextColored(dot_col, "● %s", left.c_str());
    // 悬停看完整说明（连接失败原因 / 重连第几次 / 本次会话的固定信息）。
    if (!s.session.status_detail.empty() && ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", s.session.status_detail.c_str());
    }

    if (!right.empty()) {
        same_line_right(right_w);
        ImGui::TextDisabled("%s", right.c_str());
    }
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

// ---- 顶部页面 tab：连接 / 运行日志 ----
// 样式与设置框的 tab 完全一致（用户 2026-10-18：主页不要"方角 tab + 顶部强调横条
// + tab bar 基线"那套）：BeginTabBar 什么都不给——不压 TabRounding、不画 overline、
// 不画基线。tab 的圆角等度量由 apply_theme 统一管（设置 → 外观 → 圆角），两处
// 观感因此天然一致，改圆角时一起变。
// 两个页面互斥：一个 BeginTabItem 命中才绘制，另一个完全不提交——运行日志的
// 滚动位置 / 快照缓存因此可以保持，切回来还是原样。
void draw_page_tabs(UiState& s)
{
    // 请求位（AQUA_GUI_PAGE）：命中一次即消耗，之后用户点哪页就是哪页。
    const int request = s.page_request;
    s.page_request = -1;

    if (ImGui::BeginTabBar("##pages")) {
        const ImGuiTabItemFlags connect_flags = (request == 0) ? ImGuiTabItemFlags_SetSelected : 0;
        const ImGuiTabItemFlags log_flags = (request == 1) ? ImGuiTabItemFlags_SetSelected : 0;
        if (ImGui::BeginTabItem("连接", nullptr, connect_flags)) {
            // 上半：连接卡片（AutoResizeY，自然高度）；下半：遥测面板吃满剩余。
            draw_connection_card(s);
            draw_telemetry_panel(s);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("运行日志", nullptr, log_flags)) {
            draw_log_panel(s);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
}

} // namespace

// ---- 主窗口 ----
// 布局（紧凑的竖向小工具，2026-10-18 定稿）：
//   菜单栏（viewport 级）
//   └ 主窗口
//     ├ 页面 tab：连接 | 运行日志（整页切换；样式与设置框 tab 一致）
//     │   ├ 连接页：连接卡片（地址 / 客户端名 / 设备 / 预设 / 大按钮）
//     │   │          + 遥测面板（音频格式 + 缓冲卡 + 链路卡，吃满剩余高度）
//     │   └ 日志页：工具栏（级别 / 自动滚动 / 导出 / 清空）+ 基础日志列表
//     ├ Separator
//     └ 状态栏（阶段 · 数据源 ……………… 运行时长）
void draw_main_window(UiState& s)
{
    MainWindowState& view = main_window_state();

    // 演示模式首次启动：phase 置 Connected + seed 一批日志（只做一次）。
    if (s.demo && !view.demo_seeded) {
        view.demo_seeded = true;
        s.phase = UiPhase::Connected;
        s.connected_at = ImGui::GetTime();
        seed_demo_logs();
    }

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
    const float chrome_h = ImGui::GetFrameHeight() + ImGui::GetTextLineHeightWithSpacing();
    ImGui::BeginChild("##content", ImVec2(0.0f, -chrome_h), ImGuiChildFlags_None);

    // 菜单栏与 tab 条之间留一个标准块间距（用户 2026-10-16 反馈："连接和上面的
    // 菜单栏中间没有间隔"）。主窗口 WindowPadding.y 仍压成 0（否则底部状态栏
    // 也会被顶离底缘），只给内容区顶部加一个偏移。
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + ImGui::GetStyle().WindowPadding.y);

    draw_page_tabs(s);

    ImGui::EndChild();

    ImGui::Separator();
    draw_status_bar(s);

    ImGui::End();

    // 附加窗口（模态）：统一设置框（外观 / 统计 / 高级 三个 tab）+ 关于。
    draw_settings_window(s);
    draw_about_window(s);
}

} // namespace aqua::gui
