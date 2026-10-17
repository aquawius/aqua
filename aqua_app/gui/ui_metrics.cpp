#include "ui_panels.h"

#include "ui_demo.h"
#include "ui_state.h"

#include <imgui.h>

#include <cfloat>
#include <cstdio>

namespace aqua::gui {
namespace {

// 6 个指标的单一口径表：标签 / 说明 / 单位格式。
// 字段口径与 ClientDiagnosticsSnapshot 对齐（会话接入后从这里取数）：
//   water_level / lead_ms / target_ms / estimator_jitter_ms / 丢包率 / underrun_ratio
// 卡片里的指标条（简表）与设置→统计（详表）共用同一份，改口径只改这里。
struct MetricRow {
    const char* label;
    const char* desc;
};

constexpr MetricRow kMetricRows[6] = {
    { "缓冲水位", "JitterBuffer 当前占用槽位比例" },
    { "领先",     "缓冲领先播放的时间量" },
    { "目标",     "自适应目标（随网络抖动调整）" },
    { "网络抖动", "到达抖动的平滑估计（EWMA）" },
    { "丢包",     "音频包丢失率" },
    { "欠载",     "缓冲耗尽导致播放中断的比例" },
};

constexpr int kMetricCount = static_cast<int>(sizeof(kMetricRows) / sizeof(kMetricRows[0]));

// 等宽字号 = UI 基准字号 × 0.875（16 → 14，与早期手工调校值一致）。
// 注意 PushFont 接收的是"未缩放"基准值：style.FontScaleDpi 会统一再乘，
// 所以这里直接用 UiState.font_size 推导，不掺 DPI。
// 仅指标数值用；日志列表字号是独立的（UiState::log_font_size）。
constexpr float kMonoRatio = 0.875f;

float mono_size(const UiState& s)
{
    return static_cast<float>(s.font_size) * kMonoRatio;
}

// 6 个指标的当前值：演示模式给拟真值，否则全 "-"（会话接入后从 session 取）。
void metrics_current_values(const UiState& s, char (&values)[kMetricCount][24])
{
    const bool live = (s.phase == UiPhase::Connected);
    if (live && s.demo) {
        double v[6];
        fill_demo_tiles(v);
        std::snprintf(values[0], sizeof(values[0]), "%.0f %%", v[0] * 100.0);
        std::snprintf(values[1], sizeof(values[1]), "%.1f ms", v[1]);
        std::snprintf(values[2], sizeof(values[2]), "%.1f ms", v[2]);
        std::snprintf(values[3], sizeof(values[3]), "%.1f ms", v[3]);
        std::snprintf(values[4], sizeof(values[4]), "%.2f %%", v[4]);
        std::snprintf(values[5], sizeof(values[5]), "%.2f %%", v[5] * 100.0);
    } else {
        for (auto& row : values) {
            std::snprintf(row, sizeof(row), "%s", "-");
        }
    }
}

} // namespace

// ---- 卡片里的指标条（简表：6 列等宽，第一行标签、第二行数值）----
void draw_metrics_strip(const UiState& s)
{
    char values[kMetricCount][24];
    metrics_current_values(s, values);

    if (ImGui::BeginTable("##metrics", kMetricCount,
            ImGuiTableFlags_Borders | ImGuiTableFlags_SizingStretchSame)) {
        for (int i = 0; i < kMetricCount; ++i) {
            ImGui::TableNextColumn();
            ImGui::Text("%s", kMetricRows[i].label);
        }
        ImGui::TableNextRow();
        for (int i = 0; i < kMetricCount; ++i) {
            ImGui::TableNextColumn();
            ImGui::PushFont(s.font_mono, mono_size(s));
            ImGui::TextUnformatted(values[i]);
            ImGui::PopFont();
        }
        ImGui::EndTable();
    }
}

// ---- 设置 → 统计（详表：指标 / 当前值 / 口径说明）----
// 与卡片的简表同源（kMetricRows），多一列说明——"统计"面板的职责就是把
// 卡片上那几个数字讲清楚。实时曲线随会话层接入后补（数据源同 ClientDiagnostics）。
void draw_stats_tab(const UiState& s)
{
    char values[kMetricCount][24];
    metrics_current_values(s, values);

    const bool live = (s.phase == UiPhase::Connected);
    ImGui::TextDisabled(live ? "会话进行中：以下为实时指标（演示数据）。"
                             : "未连接：连接后显示实时指标。");
    ImGui::Dummy(ImVec2(0.0f, 4.0f));

    if (ImGui::BeginTable("##stats", 3,
            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg
                | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("指标", ImGuiTableColumnFlags_WidthFixed, 96.0f);
        ImGui::TableSetupColumn("当前值", ImGuiTableColumnFlags_WidthFixed, 96.0f);
        ImGui::TableSetupColumn("说明", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        for (int i = 0; i < kMetricCount; ++i) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(kMetricRows[i].label);
            ImGui::TableNextColumn();
            ImGui::PushFont(s.font_mono, mono_size(s));
            ImGui::TextUnformatted(values[i]);
            ImGui::PopFont();
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", kMetricRows[i].desc);
        }
        ImGui::EndTable();
    }

    ImGui::Dummy(ImVec2(0.0f, 4.0f));
    ImGui::TextDisabled("数值口径与 CLI / Android 的诊断一致；实时曲线随会话层接入后提供。");
}

} // namespace aqua::gui
