#include "ui_panels.h"

#include "ui_demo.h"
#include "ui_state.h"

#include <imgui.h>

#include <algorithm>
#include <cfloat>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <string>

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
    { "缺口率",   "接收序列缺口率（乱序与丢包都计入）" },
    { "欠载",     "缓冲耗尽导致播放中断的比例" },
};

constexpr int kMetricCount = static_cast<int>(sizeof(kMetricRows) / sizeof(kMetricRows[0]));

// core 的计数器是 std::uint64_t；"%llu" 要的是 unsigned long long。统一转一次，
// 免得每个调用点都写 static_cast 长串，也避免 MSVC /W4 的格式化宽度告警。
constexpr unsigned long long u64(std::uint64_t v)
{
    return static_cast<unsigned long long>(v);
}

// 等宽字号 = UI 基准字号 × 0.875（16 → 14，与早期手工调校值一致）。
// 注意 PushFont 接收的是"未缩放"基准值：style.FontScaleDpi 会统一再乘，
// 所以这里直接用 UiState.font_size 推导，不掺 DPI。
// 仅指标数值用；日志列表字号是独立的（UiState::log_font_size）。
// （定义在匿名 namespace 之外：会话卡 ui_session.cpp 也用它，见 ui_panels.h）
constexpr float kMonoRatio = 0.875f;

} // namespace

float mono_size(const UiState& s)
{
    return static_cast<float>(s.font_size) * kMonoRatio;
}

// 健康色：把"数字变小"变成"颜色变坏"，扫一眼就知道该不该管。
//   水位 < 12% —— 欠载前兆（与遥测曲线的警戒线同一阈值），红；
//   水位 < 25% —— 低水位，黄；其余正常前景色（跟随主题）。
// 指标条与遥测曲线共用（声明在 ui_panels.h），保证数字与图形同色。
ImVec4 water_color(double level)
{
    if (level < 0.12) {
        return ImVec4(0.94f, 0.42f, 0.38f, 1.0f);
    }
    if (level < 0.25) {
        return ImVec4(0.93f, 0.80f, 0.32f, 1.0f);
    }
    return ImGui::GetStyleColorVec4(ImGuiCol_Text);
}

namespace {

// 欠载占比：> 5% 红（听感已受影响）、> 2% 黄（偶发），其余正常。
ImVec4 underrun_color(double ratio)
{
    if (ratio > 0.05) {
        return ImVec4(0.94f, 0.42f, 0.38f, 1.0f);
    }
    if (ratio > 0.02) {
        return ImVec4(0.93f, 0.80f, 0.32f, 1.0f);
    }
    return ImGui::GetStyleColorVec4(ImGuiCol_Text);
}

// 6 个指标的当前值 + 健康色。数据来源三选一，绝不残留上一次会话的数字：
//   演示模式（AQUA_GUI_DEMO）→ 拟真动画值；
//   真实会话（ClientSession 有诊断快照）→ core 的 ClientDiagnosticsSnapshot；
//   其余（未连接 / 断开中 / 失败）→ 全 "-"（颜色为 disabled）。
void metrics_current_values(const UiState& s, char (&values)[kMetricCount][24],
    ImVec4 (&colors)[kMetricCount])
{
    const bool live = (s.phase == UiPhase::Connected);
    for (int i = 0; i < kMetricCount; ++i) {
        colors[i] = ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
    }
    if (live && s.demo) {
        double v[6];
        fill_demo_tiles(v);
        std::snprintf(values[0], sizeof(values[0]), "%.1f %%", v[0] * 100.0);
        std::snprintf(values[1], sizeof(values[1]), "%.1f ms", v[1]);
        std::snprintf(values[2], sizeof(values[2]), "%.1f ms", v[2]);
        std::snprintf(values[3], sizeof(values[3]), "%.1f ms", v[3]);
        std::snprintf(values[4], sizeof(values[4]), "%.2f %%", v[4]);
        std::snprintf(values[5], sizeof(values[5]), "%.2f %%", v[5] * 100.0);
        colors[0] = water_color(v[0]);
        colors[5] = underrun_color(v[5]);
        return;
    }
    if (live && s.session.live) {
        std::snprintf(values[0], sizeof(values[0]), "%.1f %%", s.session.water_level * 100.0);
        std::snprintf(values[1], sizeof(values[1]), "%.1f ms", s.session.lead_ms);
        std::snprintf(values[2], sizeof(values[2]), "%.1f ms", s.session.target_ms);
        std::snprintf(values[3], sizeof(values[3]), "%.1f ms", s.session.jitter_ms);
        std::snprintf(values[4], sizeof(values[4]), "%.2f %%", s.session.gap_ratio * 100.0);
        std::snprintf(values[5], sizeof(values[5]), "%.2f %%", s.session.underrun_ratio * 100.0);
        colors[0] = water_color(s.session.water_level);
        colors[5] = underrun_color(s.session.underrun_ratio);
        return;
    }
    for (auto& row : values) {
        std::snprintf(row, sizeof(row), "%s", "-");
    }
}

} // namespace

// ---- 遥测面板里的指标网格 ----
// 版式（2026-10-18 第三轮）：3 组 / 行 × 2 行，每组 = 「标签 + 数值」同一行。
//
//   缓冲水位 42.1 %    领先 26.0 ms    目标 26.8 ms
//   网络抖动  3.2 ms   缺口率 0.01 %   欠载  0.00 %
//
// 为什么不用"标签一行、数值一行"的堆叠格：那样 6 个指标要吃掉 ~100px，曲线只剩
// 60px，整块面板头重脚轻。改成同行后指标只占 2 行文本（~56px），曲线拿到 ~110px，
// 而且标签 / 数值各自成一条**竖线**——3 组共 6 条竖线，横平竖直。
//
// 用表格而不是手工排版：标签列定宽（按最长标签实测，字号变了跟着变）、数值列
// 三等分拉伸，ImGui 自己保证列对齐；NoPadOuterX 让第一列的标签贴住内容左缘，
// 于是指标表、分节线、曲线盒、页脚**共用同一条左缘**。
// value_x 回填"数值列"的窗口内 X（实测），页脚的音频格式值对齐到这条竖线。
void draw_metrics_grid(const UiState& s, float* value_x)
{
    char values[kMetricCount][24];
    ImVec4 colors[kMetricCount];
    metrics_current_values(s, values, colors);

    const ImGuiStyle& st = ImGui::GetStyle();
    float label_w = 0.0f;
    for (const MetricRow& row : kMetricRows) {
        label_w = std::max(label_w, ImGui::CalcTextSize(row.label).x);
    }
    label_w += st.CellPadding.x;

    // 每行 3 组 → 6 列：偶数列 = 标签（定宽），奇数列 = 数值（等分拉伸）。
    // 必须用 SizingStretchSame：SizingStretchProp 是"按内容宽度比例"分配，数值列
    // 会因 "26.0 ms"（7 字符）比 "42.1 %"（6 字符）宽而被拉成不同宽度，三组的
    // 数值起点就差了 1~17px（2026-10-18 实测），横平竖直立刻破功。
    constexpr int kCols = 3;
    const ImGuiTableFlags flags = ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_NoPadOuterX;
    if (ImGui::BeginTable("##metrics", kCols * 2, flags)) {
        for (int c = 0; c < kCols * 2; ++c) {
            if ((c % 2) == 0) {
                ImGui::TableSetupColumn("##label", ImGuiTableColumnFlags_WidthFixed, label_w);
            } else {
                ImGui::TableSetupColumn("##value", ImGuiTableColumnFlags_WidthStretch, 1.0f);
            }
        }

        for (int i = 0; i < kMetricCount; ++i) {
            if ((i % kCols) == 0) {
                ImGui::TableNextRow();
            }
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", kMetricRows[i].label);
            ImGui::TableNextColumn();
            if (i == 0 && value_x != nullptr) {
                *value_x = ImGui::GetCursorPosX();
            }
            ImGui::PushFont(s.font_mono, mono_size(s));
            ImGui::TextColored(colors[i], "%s", values[i]);
            ImGui::PopFont();
        }
        ImGui::EndTable();
    }
}

namespace {

// 「键 / 值」一行：值用等宽字（数字对齐），fmt 是 printf 风格。
void stat_row(const UiState& s, const char* key, const char* fmt, ...)
{
    char buf[64];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::TextUnformatted(key);
    ImGui::TableNextColumn();
    ImGui::PushFont(s.font_mono, mono_size(s));
    ImGui::TextUnformatted(buf);
    ImGui::PopFont();
}

// 一张两列统计表的开 / 收（各分节共用同一宽度：键列固定 148px）。
// 尺寸策略一律 SizingStretchSame + 显式权重 1.0：值列只有一列，"Same" 就等于
// "吃掉剩余宽度"。**不要**靠 SizingStretchProp 的"按内容自动定权"——那条路要
// 等 imgui 在后续帧量完内容宽度，头几帧权重还是 0，值列会被压成几个像素（设置
// →统计的「说明」列、设置→高级的滑条列都这么塌过，2026-10-18 截图实测）。
bool begin_stat_table(const char* id)
{
    if (!ImGui::BeginTable(id, 2,
            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg
                | ImGuiTableFlags_SizingStretchSame)) {
        return false;
    }
    ImGui::TableSetupColumn("key", ImGuiTableColumnFlags_WidthFixed, 148.0f);
    ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch, 1.0f);
    return true;
}

// 字节数 → "1.23 MB" 这类人话（收 / 发字节量在会话里能涨到 GB，裸数字读不出大小）。
const char* format_bytes(std::uint64_t bytes, char (&buf)[32])
{
    if (bytes < 1024) {
        std::snprintf(buf, 32, "%llu B", static_cast<unsigned long long>(bytes));
    } else if (bytes < 1024 * 1024) {
        std::snprintf(buf, 32, "%.1f KB", static_cast<double>(bytes) / 1024.0);
    } else {
        std::snprintf(buf, 32, "%.2f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    }
    return buf;
}

} // namespace

// ---- 设置 → 统计 ----
// 三块：**实时指标**（连接页那 6 个数字 + 口径说明）+ **会话固定信息** +
// **core 诊断快照的分节明细**（网络 / 传输 / 心跳 / 抖动缓冲）。
// 数据源是会话层整份透传的 ClientDiagnosticsSnapshot（UiState::session.diag），
// 字段口径见 client_diagnostics_snapshot.h；UI 不推演、只搬运与格式化。
// live=false 时那整份快照是上次会话的残留，**一律不显示**（门在下面）。
void draw_stats_tab(const UiState& s)
{
    char values[kMetricCount][24];
    ImVec4 colors[kMetricCount];
    metrics_current_values(s, values, colors);

    const bool live = (s.phase == UiPhase::Connected && s.session.live);
    if (s.demo) {
        ImGui::TextDisabled("演示模式：以下为拟真数据（明细分节需要真实会话）。");
    } else if (live) {
        ImGui::TextDisabled("会话进行中：快照每 500 ms 刷新一次。");
    } else {
        ImGui::TextDisabled("未连接：连接后显示实时指标与明细。");
    }
    ImGui::Dummy(ImVec2(0.0f, 4.0f));

    // ---- 实时指标（与连接页卡片同源，多一列口径说明）----
    ImGui::SeparatorText("实时指标");
    if (ImGui::BeginTable("##stats", 3,
            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg
                | ImGuiTableFlags_SizingStretchSame)) {
        ImGui::TableSetupColumn("指标", ImGuiTableColumnFlags_WidthFixed, 92.0f);
        ImGui::TableSetupColumn("当前值", ImGuiTableColumnFlags_WidthFixed, 88.0f);
        ImGui::TableSetupColumn("说明", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableHeadersRow();
        for (int i = 0; i < kMetricCount; ++i) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(kMetricRows[i].label);
            ImGui::TableNextColumn();
            ImGui::PushFont(s.font_mono, mono_size(s));
            ImGui::TextColored(colors[i], "%s", values[i]);
            ImGui::PopFont();
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", kMetricRows[i].desc);
        }
        ImGui::EndTable();
    }

    // 演示模式没有真实快照：只给到指标为止（编一份假明细是自欺）。
    if (s.demo) {
        ImGui::Dummy(ImVec2(0.0f, 4.0f));
        ImGui::TextDisabled("明细分节读的是 core 的诊断快照，演示模式没有真实会话，故不显示。");
        return;
    }
    if (!live) {
        return; // 未连接 / 无快照：不显示残留数字
    }

    const aqua::diagnostics::ClientDiagnosticsSnapshot& d = s.session.diag;
    char hex[24];
    std::snprintf(hex, sizeof(hex), "0x%08llX",
        static_cast<unsigned long long>(s.session.session_id));

    // ---- 会话固定信息 ----
    ImGui::SeparatorText("会话");
    if (begin_stat_table("##stats_session")) {
        stat_row(s, "会话号", "%s", hex);
        stat_row(s, "数据面", "%s",
            s.session.data_plane.empty() ? "-" : s.session.data_plane.c_str());
        char rate[24];
        char chans[24];
        std::snprintf(rate, sizeof(rate), "%d Hz", s.session.sample_rate);
        std::snprintf(chans, sizeof(chans), "%d", s.session.channels);
        stat_row(s, "采样率", "%s", s.session.sample_rate > 0 ? rate : "-");
        stat_row(s, "声道", "%s", s.session.channels > 0 ? chans : "-");
        stat_row(s, "运行时长", "%s",
            format_duration(ImGui::GetTime() - s.connected_at).c_str());
        stat_row(s, "播放", "%s", d.playback_running ? "运行中" : "未运行");
        ImGui::EndTable();
    }
    if (!s.session.audio_format.empty()) {
        ImGui::TextDisabled("%s", s.session.audio_format.c_str());
    }

    // ---- 网络观测（JitterEstimator：只观测、不驱动水位）----
    ImGui::SeparatorText("网络");
    if (begin_stat_table("##stats_net")) {
        stat_row(s, "到达抖动", "%.2f ms", d.net.estimator_jitter_ms);
        stat_row(s, "路径底噪", "%.2f ms", d.net.estimator_base_delay_ms);
        stat_row(s, "当前 transit", "%.2f ms", d.net.estimator_transit_ms);
        stat_row(s, "transit 电平", "%.2f ms", d.net.estimator_transit_level_ms);
        stat_row(s, "乱序包", "%llu", u64(d.net.estimator_reordered_packets));
        stat_row(s, "迟到包", "%llu", u64(d.net.estimator_late_packets));
        stat_row(s, "重复包", "%llu", u64(d.net.estimator_duplicate_packets));
        stat_row(s, "序列缺口事件", "%llu", u64(d.net.rx_audio_sequence_gap_events));
        stat_row(s, "缺失帧", "%llu", u64(d.net.rx_audio_sequence_missing_frames));
        stat_row(s, "已收音频帧", "%llu", u64(d.net.audio_frames_accepted));
        stat_row(s, "畸形包", "%llu", u64(d.net.malformed_datagrams));
        ImGui::EndTable();
    }

    // ---- UDP 传输 ----
    ImGui::SeparatorText("传输");
    if (begin_stat_table("##stats_transport")) {
        char b1[32];
        char b2[32];
        stat_row(s, "收包", "%llu", u64(d.net.transport.rx_packets));
        stat_row(s, "收字节", "%s", format_bytes(d.net.transport.rx_bytes, b1));
        stat_row(s, "收包错误", "%llu", u64(d.net.transport.rx_errors));
        stat_row(s, "对端不可达", "%llu", u64(d.net.transport.rx_unreachable));
        stat_row(s, "发包", "%llu", u64(d.net.transport.tx_packets));
        stat_row(s, "发字节", "%s", format_bytes(d.net.transport.tx_bytes, b2));
        stat_row(s, "发送丢弃", "%llu", u64(d.net.transport.tx_dropped));
        ImGui::EndTable();
    }

    // ---- heartbeat（建连握手 + 稳态探活）----
    ImGui::SeparatorText("心跳");
    if (begin_stat_table("##stats_hb")) {
        stat_row(s, "ACK 总数", "%llu", u64(d.net.heartbeat_ack_count));
        stat_row(s, "连续未回应", "%u", d.net.heartbeat_ack_misses);
        stat_row(s, "距上次 ACK", "%lld ms",
            static_cast<long long>(d.net.heartbeat_ack_age_ms));
        ImGui::EndTable();
    }

    // ---- 抖动缓冲 ----
    ImGui::SeparatorText("抖动缓冲");
    if (begin_stat_table("##stats_jb")) {
        const auto& jb = d.jitter_buffer;
        stat_row(s, "水位", "%.0f %%", jb.water_level * 100.0);
        stat_row(s, "占用 / 容量", "%u / %u 槽", jb.used_slots, jb.capacity_slots);
        stat_row(s, "领先", "%.1f ms（%u 槽）", jb.lead_ms, jb.lead_slots);
        stat_row(s, "目标", "%.1f ms（%u 槽）", jb.target_ms, jb.target_slots);
        stat_row(s, "播放序列", "%llu", u64(jb.play_sequence));
        stat_row(s, "最高收到序列", "%llu", u64(jb.highest_received_sequence));
        stat_row(s, "欠载次数", "%llu", u64(jb.underrun_events));
        stat_row(s, "欠载帧", "%llu", u64(jb.underrun_frames));
        stat_row(s, "欠载占比", "%.2f %%", jb.underrun_ratio * 100.0);
        stat_row(s, "掩盖 / 饱和退回", "%llu / %llu 槽",
            u64(jb.concealed_slots), u64(jb.concealed_saturated_slots));
        stat_row(s, "Fill / Drop 占比", "%.2f %% / %.2f %%",
            jb.fill_duty * 100.0, jb.drop_duty * 100.0);
        stat_row(s, "重锚次数", "%llu", u64(jb.reanchor_count));
        ImGui::EndTable();
    }

    ImGui::Dummy(ImVec2(0.0f, 4.0f));
    ImGui::TextDisabled("字段口径与 CLI / Android 的诊断一致（同一份 ClientDiagnosticsSnapshot）。");
}

} // namespace aqua::gui
