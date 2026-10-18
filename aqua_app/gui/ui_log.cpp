#include "ui_panels.h"

#include "log_buffer.h"
#include "ui_layout.h"
#include "ui_state.h"

#include <imgui.h>

#include <cstddef>
#include <cstdint>
#include <vector>

// 运行日志面板：**基础日志显示**。
//
// 2026-10-18 简化：之前那版（连续重复折叠 / 逐段复刻 spdlog 终端配色 / 每行
// 4 次 Text + 3 次 SameLine）功能上很花，代价也实在：
//   · 折叠要给每一行现算"模板串"（把数字抹成 #），2000 行 × 每次快照；
//   · 2000 行全部提交给 ImGui，一行 8 次绘制调用 = 1.6 万次/帧。
// 日志本来只是"出问题时能翻"的东西，这里退回最朴素的写法：
//   · 一行日志 = 一次 Text（级别决定颜色），不拆段、不折叠；
//   · 用 ImGuiListClipper 只画视口里那几十行（这是性能的关键）；
//   · 快照仍带缓存：LogBuffer 的 version / 级别变了才重新拷贝。
// 功能保留四项：级别过滤、自动滚动（跟随尾部）、清空、等宽字号。

namespace aqua::gui {
namespace {

// 显示色：只给 warn / error 上色，其余用默认前景色。
// "扫一眼能看见问题"就够了；整屏按级别花花绿绿反而看不出重点。
ImVec4 level_color(int level)
{
    switch (level) {
    case kLogWarn:
        return ImVec4(0.93f, 0.80f, 0.32f, 1.0f);
    case kLogError:
        return ImVec4(0.94f, 0.42f, 0.38f, 1.0f);
    default:
        return ImGui::GetStyleColorVec4(ImGuiCol_Text);
    }
}

// 日志面板的 UI 线程私有状态。
struct LogPanelState {
    // 快照缓存：LogBuffer::version 或过滤级别变了才重取（避免每帧拷贝全部行）。
    std::vector<LogLine> lines;
    std::uint64_t version = 0;
    int level = -1;

    // ---- 跟随尾部（不是"每帧无脑滚到底"）----
    // 只在**来了新行**且**上一帧还在底部**时才吸底：手动上滚去看历史时不会被
    // 拽回去。stick 用上一帧的滚动位置判定（不用本帧的，否则内容一增长就会
    // 误判成"用户滚走了"而脱钩）。重新勾选时吸底一次 = "回到现在"。
    std::uint64_t shown_version = 0; // 上一帧渲染时的日志版本
    float last_y = 0.0f;             // 上一帧结束时的滚动位置
    float last_max_y = 0.0f;         // 上一帧结束时的最大滚动量
    bool autoscroll_last = true;     // 上一帧的勾选状态
};

LogPanelState& log_panel_state()
{
    static LogPanelState state;
    return state;
}

} // namespace

// ---- 运行日志面板 ----
// 结构：Borders 盒子内自上而下 = 工具栏一行 + 日志列表（吃满剩余高度）。
void draw_log_panel(UiState& s)
{
    // 返回值 = 本帧面板是否可见（窗口被压到没有剩余高度时为 false）：false 时
    // 只跳过内容，EndChild 仍然要调——子窗口在 BeginChild 里已经压栈了。
    if (!ImGui::BeginChild("##log_panel", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders)) {
        ImGui::EndChild();
        return;
    }

    // ---- 工具栏：级别过滤 / 自动滚动 / ……… / 清空 ----
    // 字号在「设置 → 外观 → 运行日志」里调（日志是长期盯的东西，调好了不该在
    // 主界面占一个控件的位置）。
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
    ImGui::SameLine(0.0f, 18.0f);
    ImGui::Checkbox("自动滚动", &s.log_autoscroll);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("跟随尾部：有新日志且你正停在底部时才自动滚到底。\n"
                          "手动向上滚会暂停跟随（不再被拽回去），滚回底部即恢复。");
    }

    // 右侧：清空（右对齐，与工具栏同一行）。
    const ImGuiStyle& st = ImGui::GetStyle();
    const float cw = ImGui::CalcTextSize("清空").x + st.FramePadding.x * 2.0f;
    same_line_right(cw);
    if (ImGui::Button("清空")) {
        log_buffer().clear();
    }

    // ---- 日志列表 ----
    LogBuffer& log = log_buffer();
    LogPanelState& view = log_panel_state();
    const std::uint64_t ver = log.version();
    if (ver != view.version || view.level != s.log_min_level) {
        view.lines = log.snapshot(s.log_min_level);
        view.version = ver;
        view.level = s.log_min_level;
    }

    // 工具栏最后一个控件提交后光标已在下一行，列表直接从当前光标开始
    // （不要再 NewLine：会多推一行文本高度，见 2026-10-16 的教训）。
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::GetStyleColorVec4(ImGuiCol_FrameBg));
    const float list_h = ImGui::GetContentRegionAvail().y;
    if (list_h > 24.0f && ImGui::BeginChild("##log_lines", ImVec2(0.0f, list_h))) {
        ImGui::PushFont(s.font_mono, static_cast<float>(s.log_font_size));
        if (view.lines.empty()) {
            ImGui::TextDisabled("（暂无日志：连接服务器后这里会实时输出 core 的日志）");
        } else {
            // 只画视口里的行：2000 行缓冲也只提交几十个 Text。
            // 行高固定 = 单行（不换行），所以 clipper 的等高分块是准的；
            // 超宽的行按窗口裁剪（日志正文基本都在一屏内）。
            ImGuiListClipper clipper;
            clipper.Begin(static_cast<int>(view.lines.size()));
            while (clipper.Step()) {
                for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                    const LogLine& line = view.lines[static_cast<std::size_t>(i)];
                    ImGui::TextColored(level_color(line.level), "[%s] [%s] [%s] %s",
                        line.stamp.c_str(), line.logger.c_str(), line.level_text.c_str(),
                        line.text.c_str());
                    // 单行不换行（clipper 的等高分块要求行高固定），超宽的行按
                    // 窗口右缘裁掉——悬停看全文，信息不丢。
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("%s", line.text.c_str());
                    }
                }
            }
        }
        ImGui::PopFont();

        // 跟随尾部：见 LogPanelState 的注释。
        const bool grew = (ver != view.shown_version);
        const bool at_bottom = (view.last_y >= view.last_max_y - 2.0f);
        if (s.log_autoscroll && !view.autoscroll_last) {
            ImGui::SetScrollHereY(1.0f); // 重新勾选：立刻回到最新
            view.last_y = ImGui::GetScrollMaxY();
        } else if (s.log_autoscroll && grew && at_bottom) {
            ImGui::SetScrollHereY(1.0f);
            view.last_y = ImGui::GetScrollMaxY(); // 下一帧按"已在底部"判定
        } else {
            view.last_y = ImGui::GetScrollY();
        }
        view.last_max_y = ImGui::GetScrollMaxY();
        view.shown_version = ver;
        view.autoscroll_last = s.log_autoscroll;

        ImGui::EndChild();
    }
    ImGui::PopStyleColor();

    ImGui::EndChild();
}

} // namespace aqua::gui
