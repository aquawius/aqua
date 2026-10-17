#include "ui_panels.h"

#include "log_buffer.h"
#include "ui_layout.h"
#include "ui_state.h"

#include <imgui.h>

#include <cstdint>
#include <vector>

namespace aqua::gui {
namespace {

const char* level_tag(int level)
{
    switch (level) {
    case kLogDebug: return "DEBUG";
    case kLogWarn:  return "WARN";
    case kLogError: return "ERROR";
    default:        return "INFO";
    }
}

// ---- 日志面板的 UI 线程私有状态 ----
// 快照缓存：LogBuffer 的 version / 级别过滤 / 字号任一变化才重新 snapshot
// （避免每帧拷贝全部行）。字号也参与失效：PushTextWrapPos(0) 的换行宽度按
// 当前字体度量，不失效会出现"拖了滑条日志不重排"。
// 初始 version=1 / level=-1 / font=-1 保证首帧一定取一次。
struct LogPanelState {
    std::vector<LogLine> lines;
    std::uint64_t snapshot_version = 1;
    int snapshot_level = -1;
    int snapshot_font = -1;
};

LogPanelState& log_panel_state()
{
    static LogPanelState state;
    return state;
}

} // namespace

// ---- 运行日志面板 ----
// 结构：Borders 盒子（与连接卡片一致的视觉语法：tab 坐在盒子顶边框上）内
// 自上而下 = 工具栏一行 + 日志列表（吃满剩余高度）。
void draw_log_panel(UiState& s)
{
    // 必须判返回值：窗口被压到没有剩余高度时 BeginChild 返回 false，此时若继续
    // 提交内容，控件会落到父窗口、随后的 EndChild 配对错乱（探针实测矮窗口下
    // "Must call EndChild() and not End()" / "Missing EndTabBar()" 刷屏，release
    // 构建里可能直接 assert 崩）。返回 false = 整个面板不可见，正合语义。
    // Borders 的代价是 child padding 从 0 涨到 (14,12)（imgui.cpp:7770 的零
    // padding 规则要求边框为 0）——那就是盒子的内边距本身，不是额外开销。
    if (!ImGui::BeginChild("##log_panel", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders)) {
        return;
    }

    // ---- 工具栏（独立一行：级别过滤 / 日志字号 / 自动滚动 / 右侧清空）----
    // 曾经的坑：工具栏跟在 SeparatorText 后面 SameLine，控件骑在分隔线上，
    // 观感重叠。现在标签页自带分区标题，工具栏就是普通一行，不再叠放。
    // 用户 2026-10-16：在「级别」与「自动滚动」之间加“调整字体大小”（只作用于
    // 下面的日志输出框，与 UI 字号、指标条字号独立）。滑条同样走“独立 Text 标签
    // + 空标签滑块填充”的规避手法（1.92 滑条标签画在框右侧，见网络预设处注释）。
    static const char* const kLevelNames[] = { "Debug", "Info", "Warn", "Error" };
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("级别");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(96.0f);
    if (ImGui::BeginCombo("##log_level", kLevelNames[s.log_min_level])) {
        for (int i = 0; i < 4; ++i) {
            const bool selected = (i == s.log_min_level);
            if (ImGui::Selectable(kLevelNames[i], selected)) {
                s.log_min_level = i;
            }
            if (selected) {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("字体大小");
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("调整日志输出框的字体大小（10–20 px，仅作用于日志列表）。");
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(88.0f);
    ImGui::SliderInt("##log_font", &s.log_font_size, 10, 20, "%d px");
    ImGui::SameLine();
    ImGui::Checkbox("自动滚动", &s.log_autoscroll);

    // 右侧：清空（右对齐，与工具栏同一行）。另存为按钮按用户 2026-10-16 要求
    // 从日志工具栏移除（保存能力留给后续会话层 / 设置入口，Phase 0 不暴露）。
    const ImGuiStyle& st = ImGui::GetStyle();
    const float cw = ImGui::CalcTextSize("清空").x + st.FramePadding.x * 2.0f;
    same_line_right(cw);
    if (ImGui::Button("清空")) {
        log_buffer().clear();
    }

    // ---- 日志列表（增量缓存：version 或级别 / 字号变化才重新快照）----
    LogBuffer& log = log_buffer();
    LogPanelState& view = log_panel_state();
    const std::uint64_t ver = log.version();
    if (ver != view.snapshot_version || view.snapshot_level != s.log_min_level
        || view.snapshot_font != s.log_font_size) {
        view.lines = log.snapshot(s.log_min_level);
        view.snapshot_version = ver;
        view.snapshot_level = s.log_min_level;
        view.snapshot_font = s.log_font_size;
    }

    // 工具栏与列表之间只留一个标准行距：工具栏最后一个控件（清空按钮）提交后，
    // ItemSize 已把光标落到下一行（行高 + ItemSpacing.y = 控件底 + 7px），列表
    // child 直接从当前光标开始即可。曾经的 NewLine() 会再按一行文本高度（23px）
    // 推进，总空隙 ~30px（用户 2026-10-16 反馈“级别和下面的日志间隔过大”）。
    // 列表区用原生 FrameBg 作底（比面板底色略浅，形成内嵌感）——只复用
    // ImGuiCol_* 原生色，不引入自定义颜色。
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::GetStyleColorVec4(ImGuiCol_FrameBg));
    const float list_h = ImGui::GetContentRegionAvail().y;
    if (list_h > 24.0f && ImGui::BeginChild("##log_lines", ImVec2(0.0f, list_h))) {
        // 日志列表字号用工具栏滑条的值（默认 14 = 原 mono_size 口径），与 UI
        // 字号、指标条等宽字号相互独立。
        ImGui::PushFont(s.font_mono, static_cast<float>(s.log_font_size));
        ImGui::PushTextWrapPos(0.0f);
        if (view.lines.empty()) {
            ImGui::TextDisabled("（暂无日志）");
        }
        for (const LogLine& line : view.lines) {
            ImGui::Text("[%s] %-5s %s", line.stamp.c_str(), level_tag(line.level), line.text.c_str());
        }
        ImGui::PopTextWrapPos();
        ImGui::PopFont();
        if (s.log_autoscroll) {
            ImGui::SetScrollHereY(1.0f);
        }
        ImGui::EndChild();
    }
    ImGui::PopStyleColor();

    ImGui::EndChild();
}

} // namespace aqua::gui
