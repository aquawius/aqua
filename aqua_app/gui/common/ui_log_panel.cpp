#include "ui_log_panel.h"

#include "app_style.h"
#include "ui_layout.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <string>
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
// 功能保留五项：级别过滤、自动滚动（跟随尾部）、导出落盘、清空、等宽字号。
//
// client / server 共用：唯一的前端差异（导出目录 / 前缀 / 空列表文案）都在
// LogView 里，行为与排版完全一致。

namespace aqua::gui {
namespace {

// 显示色：与终端的观感对齐——trace 灰、debug 蓝、info 正文色、warn 黄、error 红。
// 面板整行染色，但**必须把这几个级别区分开**：trace/debug/info 全用正文色时，
// 扫日志分不出轻重。
ImVec4 level_color(int level)
{
    // 语义色跟随主题：浅色底上写死亮黄 / 亮红会糊成一片（对比度不足）。
    switch (level) {
    case kLogTrace:
        return ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
    case kLogDebug:
        // 用链接色（主题里就是"次级强调"的蓝），比写死 #6BA6FF 更稳。
        return ImGui::GetStyleColorVec4(ImGuiCol_TextLink);
    case kLogWarn:
        return semantic_warn();
    case kLogError:
        return semantic_error();
    default:
        return ImGui::GetStyleColorVec4(ImGuiCol_Text);
    }
}

// 日志面板的 UI 线程私有状态。
struct LogPanelState {
    // 缓存的**稳定序号**：通过级别过滤的行（collect_indices 填充）。只存序号，
    // 内容按需取（line_at），见 log_buffer.h 的说明。不能用 deque 下标：缓冲
    // 满了会 pop_front，两次加锁之间下标就全错位了。
    std::vector<std::uint64_t> indices;
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

// 导出：当前缓冲整份写进前端给的"用户数据目录"下带时间戳的 .log。
// 面板里的缓冲是有界环形（2000 行），要留档就得落盘——这就是 CLI --log-file 的
// GUI 版。写完在面板里回一行写明完整路径：不然用户不知道导出成功了没有、存在哪。
void export_log(const LogView& view)
{
    if (view.export_dir.empty()) {
        log_buffer().append(kLogError, "log export failed: user data directory unavailable");
        return;
    }
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &now);
#else
    localtime_r(&now, &tm);
#endif
    char name[96];
    std::snprintf(name, sizeof(name), "%s-%04d%02d%02d-%02d%02d%02d.log",
        view.export_prefix != nullptr ? view.export_prefix : "aqua",
        tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
    const std::string path = view.export_dir + name;
    // 先写盘再回日志行：写进去的内容不含"已导出"这行本身（否则文件比自己多一行）。
    if (log_buffer().save_to_file(path)) {
        log_buffer().append(kLogInfo, "log exported: " + path);
    } else {
        log_buffer().append(kLogError, "log export failed (cannot write " + path + ")");
    }
}

} // namespace

// ---- 运行日志面板 ----
// 结构：Borders 盒子内自上而下 = 工具栏一行 + 日志列表（吃满剩余高度）。
void draw_log_panel(LogView& view)
{
    // 日志是"一眼扫一屏"的东西，全局留白（14,12）对它太奢侈：外框收到 (10,8)。
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f, 8.0f));
    // 返回值 = 本帧面板是否可见（窗口被压到没有剩余高度时为 false）：false 时
    // 只跳过内容，EndChild 仍然要调——子窗口在 BeginChild 里已经压栈了。
    if (!ImGui::BeginChild("##log_panel", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders)) {
        ImGui::EndChild();
        ImGui::PopStyleVar();
        return;
    }

    // ---- 工具栏：级别过滤 / 自动滚动 / ……… / 导出 / 清空 ----
    static const char* const kLevelNames[] = { "Trace", "Debug", "Info", "Warn", "Error" };
    if (view.min_level < 0) {
        view.min_level = 0;
    }
    if (view.min_level > 4) {
        view.min_level = 4;
    }
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("级别");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(96.0f);
    if (ImGui::BeginCombo("##log_level", kLevelNames[view.min_level])) {
        for (int i = 0; i < 5; ++i) {
            const bool selected = (i == view.min_level);
            if (ImGui::Selectable(kLevelNames[i], selected)) {
                view.min_level = i;
            }
            if (selected) {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("低于此级别的日志不显示。\n"
                          "它同时驱动 core 的日志级别：要看逐包 trace 必须选 Trace。");
    }
    ImGui::SameLine(0.0f, 18.0f);
    ImGui::Checkbox("自动滚动", &view.autoscroll);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("跟随尾部：有新日志且你正停在底部时才自动滚到底。\n"
                          "手动向上滚会暂停跟随（不再被拽回去），滚回底部即恢复。");
    }

    // 右侧：导出… + 清空（右对齐，与工具栏同一行；导出在左、清空贴右缘）。
    const ImGuiStyle& st = ImGui::GetStyle();
    const float cw = ImGui::CalcTextSize("清空").x + st.FramePadding.x * 2.0f;
    const float ew = ImGui::CalcTextSize("导出…").x + st.FramePadding.x * 2.0f;
    same_line_right(cw + st.ItemSpacing.x + ew);
    if (ImGui::Button("导出…")) {
        export_log(view);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("把当前日志缓冲写入用户数据目录下的\n"
                          "%s-YYYYMMDD-HHMMSS.log（CLI 的 --log-file）。",
            view.export_prefix != nullptr ? view.export_prefix : "aqua");
    }
    ImGui::SameLine(0.0f, st.ItemSpacing.x);
    if (ImGui::Button("清空")) {
        log_buffer().clear();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("清空面板里的缓冲（内存中最多 2000 行）。\n"
                          "不影响已经「导出…」到磁盘的文件。");
    }

    // ---- 日志列表 ----
    LogBuffer& log = log_buffer();
    LogPanelState& state = log_panel_state();
    const std::uint64_t ver = log.version();
    if (ver != state.version || state.level != view.min_level) {
        // 只收集"通过过滤的行下标"，不拷内容：内容等 clipper 报到哪一行再取哪一行。
        log.collect_indices(view.min_level, state.indices);
        state.version = ver;
        state.level = view.min_level;
    }
    const int visible_count = static_cast<int>(state.indices.size());

    // 工具栏最后一个控件提交后光标已在下一行，列表直接从当前光标开始
    // （不要再 NewLine：会多推一行文本高度，见 2026-10-16 的教训）。
    // AlwaysUseWindowPadding 必须给：**没有边框的 child 默认不套 WindowPadding**
    // （imgui.h 的说明），日志正文会直接贴在列表左上角，看着像没有留白。
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::GetStyleColorVec4(ImGuiCol_FrameBg));
    // 只收**列表相对于边框的留白**（14,12 → 8,5）；行距保持主题默认——
    // 压到 1px 日志会挤成一片反而难读。
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 5.0f));
    const float list_h = ImGui::GetContentRegionAvail().y;
    if (list_h > 24.0f && ImGui::BeginChild("##log_lines", ImVec2(0.0f, list_h),
            ImGuiChildFlags_AlwaysUseWindowPadding)) {
        ImGui::PushFont(view.mono_font, static_cast<float>(view.font_size));
        if (visible_count == 0) {
            ImGui::TextDisabled("%s", view.empty_hint != nullptr ? view.empty_hint : "（暂无日志）");
        } else {
            // 只画视口里的行：2000 行缓冲也只提交几十个 Text，而且**只拷这几十行**
            // （下标在 collect_indices 里一次扫完，字符串拷贝按需）。
            // 行高固定 = 单行（不换行），所以 clipper 的等高分块是准的；
            // 超宽的行按窗口裁剪（日志正文基本都在一屏内）。
            ImGuiListClipper clipper;
            clipper.Begin(visible_count);
            while (clipper.Step()) {
                for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                    LogLine line;
                    if (!log.line_at(state.indices[static_cast<std::size_t>(i)], line)) {
                        continue; // 收集序号与读取之间这行已被挤出（极罕见）
                    }
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
        const bool grew = (ver != state.shown_version);
        const bool at_bottom = (state.last_y >= state.last_max_y - 2.0f);
        if (view.autoscroll && !state.autoscroll_last) {
            ImGui::SetScrollHereY(1.0f); // 重新勾选：立刻回到最新
            state.last_y = ImGui::GetScrollMaxY();
        } else if (view.autoscroll && grew && at_bottom) {
            ImGui::SetScrollHereY(1.0f);
            state.last_y = ImGui::GetScrollMaxY(); // 下一帧按"已在底部"判定
        } else {
            state.last_y = ImGui::GetScrollY();
        }
        state.last_max_y = ImGui::GetScrollMaxY();
        state.shown_version = ver;
        state.autoscroll_last = view.autoscroll;

        ImGui::EndChild();
    }
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();

    ImGui::EndChild();
    ImGui::PopStyleVar();
}

} // namespace aqua::gui
