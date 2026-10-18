#include "ui_panels.h"

#include "ui_demo.h"
#include "ui_state.h"

#include <aqua/net/udp/udp_config.h>

#include <imgui.h>

#include <algorithm>
#include <cfloat>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <string>

namespace aqua::gui {
namespace {

// 等宽字号 = UI 基准字号 × 0.875（16 → 14，与早期手工调校值一致）。
// 注意 PushFont 接收的是"未缩放"基准值：style.FontScaleDpi 会统一再乘，
// 所以这里直接用 UiState.font_size 推导，不掺 DPI。
constexpr float kMonoRatio = 0.875f;

// 全部指标的单一口径表：按 MetricId 的顺序排列（缓冲组 6 + 链路组 6）。
// 面板卡片、设置→统计的详表、tooltip 都从这里取，改口径只改这一处。
struct MetricSpec {
    const char* label;
    const char* desc;
};

constexpr MetricSpec kMetricSpecs[kMetricIdCount] = {
    { "缓冲水位", "JitterBuffer 当前占用槽位比例（其下方「占用」的百分比形式）" },
    { "占用", "已占用槽位 / 容量（槽）。水位百分比 = 它除以容量。" },
    { "领先", "缓冲领先播放的时间量（lead_slots 换算，与「目标」同口径）" },
    { "目标", "自适应目标（随网络抖动 / 到达峰值调整；固定模式下是钉住值）" },
    { "欠载", "缓冲耗尽、播放头拿不到真实 PCM 的帧占比" },
    { "重锚/掩盖", "时间轴重锚次数 / 用重复末帧掩盖的槽数（非零 = 能听见的中断）" },
    { "网络抖动", "到达抖动的平滑估计（RFC 3550 interarrival，只观测不驱动水位）" },
    { "缺口率", "接收序列缺口率（乱序与丢包都计入）" },
    { "到达速率", "UDP 音频包到达速率（两次快照的收包差值 / 实际间隔，平滑后）" },
    { "接收量", "本次会话累计收到的字节数（音频 + 心跳）" },
    { "乱序/迟到", "乱序到达的包 / 到达太晚已被跳过的包（迟到会直接造成掩盖）" },
    { "ACK 延迟", "距上一次心跳 ACK 的毫秒数 / 心跳周期：0..周期内锯齿跳动是正常的（相位量），"
                  "卡住不刷新或出现「缺 N 次」才是链路问题" },
};

// 口径表下标越界保护：enum 与表必须一一对应，编译期已由数组长度保证；
// 这里只把 id 转下标。
constexpr int metric_index(MetricId id)
{
    return static_cast<int>(id);
}

// 健康色阈值（水位与欠载共用；曲线也用它）。
ImVec4 underrun_color(double ratio)
{
    if (ratio > 0.05) {
        return semantic_error();
    }
    if (ratio > 0.02) {
        return semantic_warn();
    }
    return ImGui::GetStyleColorVec4(ImGuiCol_Text);
}

// 演示模式下 12 个指标的拟真值（水面数据 + 链路数据两套公式拼起来）。
struct DemoMetrics {
    double tiles[6]; // water% / lead / target / jitter / gap% / underrun
    double link[kDemoLinkCount];
};

DemoMetrics demo_metrics()
{
    DemoMetrics d;
    fill_demo_tiles(d.tiles);
    fill_demo_link(d.link);
    return d;
}

} // namespace

float mono_size(const UiState& s)
{
    return static_cast<float>(s.font_size) * kMonoRatio;
}

float mono_baseline_dy(const UiState& s)
{
    if (s.font_mono == nullptr) {
        return 0.0f;
    }
    // 先量等宽字体的 ascent（push 一下即可，baked 数据是惰性烘焙 + 缓存的）。
    ImGui::PushFont(s.font_mono, mono_size(s));
    const float mono_ascent = ImGui::GetFontBaked()->Ascent;
    ImGui::PopFont();
    const float label_ascent = ImGui::GetFontBaked()->Ascent;
    const float dy = label_ascent - mono_ascent;
    return (dy > 0.0f) ? dy : 0.0f;
}

ImVec4 water_color(double level)
{
    if (level < 0.12) {
        return semantic_error();
    }
    if (level < 0.25) {
        return semantic_warn();
    }
    return ImGui::GetStyleColorVec4(ImGuiCol_Text);
}

const char* metric_label(MetricId id)
{
    return kMetricSpecs[metric_index(id)].label;
}

const char* metric_desc(MetricId id)
{
    return kMetricSpecs[metric_index(id)].desc;
}

std::string format_bytes(std::uint64_t bytes)
{
    char buf[32];
    if (bytes < 1024) {
        std::snprintf(buf, sizeof(buf), "%llu B", static_cast<unsigned long long>(bytes));
    } else if (bytes < 1024 * 1024) {
        std::snprintf(buf, sizeof(buf), "%.1f KB", static_cast<double>(bytes) / 1024.0);
    } else {
        std::snprintf(buf, sizeof(buf), "%.2f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    }
    return buf;
}

std::string format_duration(double seconds)
{
    const int total = static_cast<int>(std::max(0.0, seconds));
    char buf[32];
    if (total < 3600) {
        std::snprintf(buf, sizeof(buf), "%02d:%02d", total / 60, total % 60);
    } else {
        std::snprintf(buf, sizeof(buf), "%d:%02d:%02d", total / 3600, (total / 60) % 60, total % 60);
    }
    return buf;
}

std::string metric_text(const UiState& s, MetricId id)
{
    // 演示模式：一套拟真值（与真实会话同量级），任何阶段都能看全界面。
    if (s.demo) {
        const DemoMetrics demo = demo_metrics();
        char buf[64];
        switch (id) {
        case MetricId::WaterPct:
            std::snprintf(buf, sizeof(buf), "%.1f %%", demo.tiles[0] * 100.0);
            break;
        case MetricId::Slots:
            std::snprintf(buf, sizeof(buf), "%.0f / %.0f 槽", demo.link[0], demo.link[1]);
            break;
        case MetricId::Lead:
            std::snprintf(buf, sizeof(buf), "%.1f ms", demo.tiles[1]);
            break;
        case MetricId::Target:
            std::snprintf(buf, sizeof(buf), "%.1f ms", demo.tiles[2]);
            break;
        case MetricId::Underrun:
            std::snprintf(buf, sizeof(buf), "%.2f %%", demo.tiles[5] * 100.0);
            break;
        case MetricId::Glitches:
            std::snprintf(buf, sizeof(buf), "%.0f / %.0f", demo.link[6], demo.link[7]);
            break;
        case MetricId::Jitter:
            std::snprintf(buf, sizeof(buf), "%.1f ms", demo.tiles[3]);
            break;
        case MetricId::Gap:
            std::snprintf(buf, sizeof(buf), "%.2f %%", demo.tiles[4]);
            break;
        case MetricId::RxRate:
            std::snprintf(buf, sizeof(buf), "%.0f 包/秒", demo.link[2]);
            break;
        case MetricId::RxBytes:
            // 拟真累计量：按包速 × 包长 × （已运行时间 + 一段"历史"）估一个，
            // 免得演示第一屏显示 0.00 MB 像没数据。
            std::snprintf(buf, sizeof(buf), "%.2f MB",
                demo.link[2] * 380.0 * (ImGui::GetTime() + 120.0) / (1024.0 * 1024.0));
            break;
        case MetricId::ReorderLate:
            std::snprintf(buf, sizeof(buf), "%.0f / %.0f", demo.link[4], demo.link[5]);
            break;
        case MetricId::AckAge:
            std::snprintf(buf, sizeof(buf), "%.0f/%.0f ms", demo.link[3],
                static_cast<double>(aqua::config::HEARTBEAT_INTERVAL.count()));
            break;
        }
        return buf;
    }

    // 真实会话：没有任何本次会话数据时一律 "-"（占位，不显示上一场残留）。
    if (s.phase != UiPhase::Connected || !s.session.live) {
        return "-";
    }
    const aqua::diagnostics::ClientDiagnosticsSnapshot& d = s.session.diag;
    const auto& jb = d.jitter_buffer;
    char buf[64];
    switch (id) {
    case MetricId::WaterPct:
        std::snprintf(buf, sizeof(buf), "%.1f %%", s.session.water_level * 100.0);
        break;
    case MetricId::Slots:
        std::snprintf(buf, sizeof(buf), "%u / %u 槽", jb.used_slots, jb.capacity_slots);
        break;
    case MetricId::Lead:
        std::snprintf(buf, sizeof(buf), "%.1f ms", s.session.lead_ms);
        break;
    case MetricId::Target:
        std::snprintf(buf, sizeof(buf), "%.1f ms", s.session.target_ms);
        break;
    case MetricId::Underrun:
        std::snprintf(buf, sizeof(buf), "%.2f %%", s.session.underrun_ratio * 100.0);
        break;
    case MetricId::Glitches:
        std::snprintf(buf, sizeof(buf), "%llu / %llu",
            static_cast<unsigned long long>(jb.reanchor_count),
            static_cast<unsigned long long>(jb.concealed_slots));
        break;
    case MetricId::Jitter:
        std::snprintf(buf, sizeof(buf), "%.1f ms", s.session.jitter_ms);
        break;
    case MetricId::Gap:
        std::snprintf(buf, sizeof(buf), "%.2f %%", s.session.gap_ratio * 100.0);
        break;
    case MetricId::RxRate:
        std::snprintf(buf, sizeof(buf), "%.0f 包/秒", s.session.rx_rate);
        break;
    case MetricId::RxBytes:
        return format_bytes(d.net.transport.rx_bytes);
    case MetricId::ReorderLate:
        std::snprintf(buf, sizeof(buf), "%llu / %llu",
            static_cast<unsigned long long>(d.net.estimator_reordered_packets),
            static_cast<unsigned long long>(d.net.estimator_late_packets));
        break;
    case MetricId::AckAge:
        // 「距上次 ACK」是**相位量**：稳态心跳每 HEARTBEAT_INTERVAL(1s) 一次，
        // 500ms 采样落在周期内的不同相位上，所以它天生在 0..1000ms 之间锯齿跳。
        // 把分母一起显示（Android 同口径），就不会被误读成"心跳只有 300ms"。
        if (d.net.heartbeat_ack_misses > 0) {
            std::snprintf(buf, sizeof(buf), "缺 %u 次", d.net.heartbeat_ack_misses);
        } else {
            std::snprintf(buf, sizeof(buf), "%.0f/%.0f ms",
                static_cast<double>(d.net.heartbeat_ack_age_ms),
                static_cast<double>(aqua::config::HEARTBEAT_INTERVAL.count()));
        }
        break;
    }
    return buf;
}

ImVec4 metric_color(const UiState& s, MetricId id)
{
    const ImVec4 normal = ImGui::GetStyleColorVec4(ImGuiCol_Text);
    const ImVec4 red = semantic_error();
    const ImVec4 yellow = semantic_warn();
    const bool placeholder = !s.demo && (s.phase != UiPhase::Connected || !s.session.live);
    if (placeholder) {
        return ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
    }

    if (s.demo) {
        const DemoMetrics demo = demo_metrics();
        switch (id) {
        case MetricId::WaterPct: return water_color(demo.tiles[0]);
        case MetricId::Slots:    return water_color(demo.link[1] > 0.0 ? demo.link[0] / demo.link[1] : 0.0);
        case MetricId::Underrun: return underrun_color(demo.tiles[5]);
        default:                 return normal;
        }
    }

    const aqua::diagnostics::ClientDiagnosticsSnapshot& d = s.session.diag;
    switch (id) {
    case MetricId::WaterPct:
        return water_color(s.session.water_level);
    case MetricId::Slots:
        return water_color(d.jitter_buffer.capacity_slots > 0
                ? static_cast<double>(d.jitter_buffer.used_slots)
                    / static_cast<double>(d.jitter_buffer.capacity_slots)
                : 0.0);
    case MetricId::Underrun:
        return underrun_color(s.session.underrun_ratio);
    case MetricId::Glitches:
        return (d.jitter_buffer.reanchor_count > 0 || d.jitter_buffer.concealed_slots > 0) ? red : normal;
    case MetricId::RxRate:
        return (s.session.rx_rate < 1.0) ? red : normal;
    case MetricId::ReorderLate:
        if (d.net.estimator_late_packets > 0) {
            return red;
        }
        return (d.net.estimator_reordered_packets > 0) ? yellow : normal;
    case MetricId::AckAge:
        if (d.net.heartbeat_ack_misses > 0) {
            return red;
        }
        return (d.net.heartbeat_ack_misses == 0 && d.net.heartbeat_ack_age_ms >= 1000) ? yellow : normal;
    default:
        return normal;
    }
}

// ---- 指标网格（面板「缓冲」卡）----
// pairs_per_row 组 / 行；每组 = 「标签（左） ……… 数值（右）」。两组之间的间距
// 由列宽均分给出，标签与数值各自成一条竖线（2 组 = 4 条竖线），横平竖直。
void draw_metric_grid(const UiState& s, const MetricId* ids, int count, int pairs_per_row,
    float width, float* value_x)
{
    if (count <= 0 || pairs_per_row <= 0 || width <= 1.0f) {
        return;
    }
    const float dy = mono_baseline_dy(s);
    // 组与组之间留一条明显的间隔：数值右对齐到每组的右缘，没有间隔的话前一组的
    // 数值会和后一组的标签贴在一起（实测会撞）。
    const float gutter = 20.0f * ((s.dpi_scale > 0.0f) ? s.dpi_scale : 1.0f);
    const float pair_w = (width - gutter * static_cast<float>(pairs_per_row - 1))
        / static_cast<float>(pairs_per_row);
    const float right_gap = 6.0f * ((s.dpi_scale > 0.0f) ? s.dpi_scale : 1.0f);

    const ImVec2 origin = ImGui::GetCursorScreenPos();
    int row = 0;
    for (int i = 0; i < count; ++i) {
        const int col = i % pairs_per_row;
        if (col == 0 && i > 0) {
            ++row;
        }
        const float row_top = origin.y + ImGui::GetTextLineHeightWithSpacing() * static_cast<float>(row);
        const float pair_x = origin.x + (pair_w + gutter) * static_cast<float>(col);
        const float value_right = pair_x + pair_w - right_gap;

        // 先提交隐形按钮拿 hover（tooltip 用），再用 draw list 精确落字。
        ImGui::SetCursorScreenPos(ImVec2(pair_x, row_top));
        ImGui::PushID(i);
        ImGui::InvisibleButton("##metric", ImVec2(std::max(pair_w - right_gap, 1.0f),
            ImGui::GetTextLineHeight()));
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", metric_desc(ids[i]));
        }
        ImGui::PopID();

        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddText(ImVec2(pair_x, row_top), ImGui::GetColorU32(ImGuiCol_TextDisabled),
            metric_label(ids[i]));

        const std::string text = metric_text(s, ids[i]);
        ImGui::PushFont(s.font_mono, mono_size(s));
        const float value_w = ImGui::CalcTextSize(text.c_str()).x;
        ImGui::PopFont();
        const float value_x_screen = value_right - value_w;
        if (i == 0 && value_x != nullptr) {
            *value_x = value_x_screen - ImGui::GetWindowPos().x;
        }
        dl->AddText(s.font_mono, mono_size(s), ImVec2(value_x_screen, row_top + dy),
            ImGui::GetColorU32(metric_color(s, ids[i])), text.c_str());
    }

    // 占位 item：让后续内容从网格下方开始（网格是自绘的，没有自己的 item 高度）。
    const int rows = (count + pairs_per_row - 1) / pairs_per_row;
    ImGui::SetCursorScreenPos(origin);
    ImGui::Dummy(ImVec2(width, ImGui::GetTextLineHeightWithSpacing() * static_cast<float>(rows)));
}

namespace {

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

// 「键 / 值」一行：值用等宽字（数字对齐）。
void stat_row_text(const UiState& s, const char* key, const std::string& text)
{
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::TextUnformatted(key);
    ImGui::TableNextColumn();
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + mono_baseline_dy(s));
    ImGui::PushFont(s.font_mono, mono_size(s));
    ImGui::TextUnformatted(text.c_str());
    ImGui::PopFont();
}

// 同上，fmt 是 printf 风格。live=false 时值一律 "-"：占位而不是上一场会话的残留
// ——统计页和连接页同一套规矩（不连接时也要显示完整的分节）。
void stat_row(const UiState& s, bool live, const char* key, const char* fmt, ...)
{
    if (!live) {
        stat_row_text(s, key, "-");
        return;
    }
    char buf[64];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    stat_row_text(s, key, buf);
}

// 采样率 → "48 kHz" / "44100 Hz"（与遥测面板的摘要同一条规则）。
std::string format_sample_rate(int hz)
{
    char buf[32];
    if (hz <= 0) {
        std::snprintf(buf, sizeof(buf), "-");
    } else if (hz % 1000 == 0) {
        std::snprintf(buf, sizeof(buf), "%d kHz", hz / 1000);
    } else {
        std::snprintf(buf, sizeof(buf), "%.1f kHz", static_cast<double>(hz) / 1000.0);
    }
    return buf;
}

// 声道数 → "立体声" / "单声道" / "N 声道"（与摘要文案一致）。
std::string format_channels(int channels)
{
    char buf[32];
    switch (channels) {
    case 0: std::snprintf(buf, sizeof(buf), "-"); break;
    case 1: std::snprintf(buf, sizeof(buf), "单声道"); break;
    case 2: std::snprintf(buf, sizeof(buf), "立体声"); break;
    default: std::snprintf(buf, sizeof(buf), "%d 声道", channels); break;
    }
    return buf;
}

} // namespace

// ---- 设置 → 统计 ----
// 完整明细，**分节与连接状态无关**：未连接时每一行都显示 "-"（占位），连上后原地
// 填入本次会话的值；这样可以一眼看出"这一页总共有哪些量"。分节顺序：
//   音频格式（会话契约）→ 实时指标（面板那 12 个数字 + 口径说明）→ 会话 →
//   网络 → 传输 → 心跳 → 抖动缓冲
// 数据源是会话层整份透传的 ClientDiagnosticsSnapshot（口径见
// client_diagnostics_snapshot.h）与会话建立结果；UI 不推演、只搬运与格式化。
void draw_stats_tab(const UiState& s)
{
    const bool demo = s.demo;
    // live = 有本次会话的真实诊断快照。断开后会话层会把所有字段清空，所以
    // "有值"与 live 等价（演示模式走自己的拟真值）。
    const bool live = (s.phase == UiPhase::Connected && s.session.live);
    const bool has_fmt = demo || live;

    if (demo) {
        ImGui::TextDisabled("演示模式：音频格式与实时指标是拟真值，其余分节为占位。");
    } else if (live) {
        ImGui::TextDisabled("会话进行中：诊断快照每 500 ms 刷新一次。");
    } else {
        ImGui::TextDisabled("未连接：以下为占位（-），连上后原地填入本次会话的数据。");
    }
    ImGui::Dummy(ImVec2(0.0f, 4.0f));

    // ---- 音频格式（会话契约；与连接页第一行同源，这里逐项拆开）----
    ImGui::SeparatorText("音频格式");
    {
        const DemoFormat demo_fmt = demo_format();
        const int sr = demo ? demo_fmt.sample_rate : s.session.sample_rate;
        const int ch = demo ? demo_fmt.channels : s.session.channels;
        const int bits = demo ? demo_fmt.bits_per_sample : s.session.bits_per_sample;
        const int kbps = demo ? demo_fmt.bitrate_kbps : s.session.bitrate_kbps;
        const int frames = demo ? demo_fmt.frame_count : s.session.frame_count;
        const double packet_ms = demo ? demo_fmt.packet_ms : s.session.packet_ms;

        char bits_txt[32];
        char kbps_txt[32];
        char frames_txt[64];
        std::snprintf(bits_txt, sizeof(bits_txt), "%d bit", bits);
        std::snprintf(kbps_txt, sizeof(kbps_txt), "%d kbps", kbps);
        std::snprintf(frames_txt, sizeof(frames_txt), "%d 帧 ≈ %.1f ms", frames, packet_ms);

        if (begin_stat_table("##stats_format")) {
            stat_row_text(s, "采样率", has_fmt ? format_sample_rate(sr) : "-");
            stat_row_text(s, "声道", has_fmt ? format_channels(ch) : "-");
            stat_row_text(s, "编码", has_fmt ? (demo ? std::string(demo_fmt.encoding) : s.session.encoding) : "-");
            stat_row_text(s, "位深", (has_fmt && bits > 0) ? std::string(bits_txt) : "-");
            stat_row_text(s, "码率", (has_fmt && kbps > 0) ? std::string(kbps_txt) : "-");
            stat_row_text(s, "帧长", (has_fmt && frames > 0) ? std::string(frames_txt) : "-");
            ImGui::EndTable();
        }
    }

    // ---- 实时指标（与连接页同源，多一列口径说明）----
    ImGui::SeparatorText("实时指标");
    const float dy = mono_baseline_dy(s);
    if (ImGui::BeginTable("##stats", 3,
            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg
                | ImGuiTableFlags_SizingStretchSame)) {
        ImGui::TableSetupColumn("指标", ImGuiTableColumnFlags_WidthFixed, 92.0f);
        ImGui::TableSetupColumn("当前值", ImGuiTableColumnFlags_WidthFixed, 108.0f);
        ImGui::TableSetupColumn("说明", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableHeadersRow();
        for (int i = 0; i < kMetricIdCount; ++i) {
            const MetricId id = static_cast<MetricId>(i);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(metric_label(id));
            ImGui::TableNextColumn();
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + dy);
            ImGui::PushFont(s.font_mono, mono_size(s));
            ImGui::TextColored(metric_color(s, id), "%s", metric_text(s, id).c_str());
            ImGui::PopFont();
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", metric_desc(id));
        }
        ImGui::EndTable();
    }

    const aqua::diagnostics::ClientDiagnosticsSnapshot& d = s.session.diag;

    // ---- 会话（本次会话的上下文；明细层面的完整版，状态栏只放一句话）----
    ImGui::SeparatorText("会话");
    if (begin_stat_table("##stats_session")) {
        char hex[24];
        if (live) {
            std::snprintf(hex, sizeof(hex), "0x%08llX",
                static_cast<unsigned long long>(s.session.session_id));
        } else {
            std::snprintf(hex, sizeof(hex), "-");
        }
        stat_row_text(s, "会话号", hex);
        stat_row_text(s, "数据面", live ? s.session.data_plane : std::string("-"));
        stat_row_text(s, "运行时长",
            live ? format_duration(ImGui::GetTime() - s.connected_at) : std::string("-"));
        stat_row(s, live, "播放", "%s", d.playback_running ? "运行中" : "未运行");
        ImGui::EndTable();
    }

    // ---- 网络观测（JitterEstimator：只观测、不驱动水位）----
    ImGui::SeparatorText("网络");
    if (begin_stat_table("##stats_net")) {
        stat_row(s, live, "到达抖动", "%.2f ms", d.net.estimator_jitter_ms);
        stat_row(s, live, "路径底噪", "%.2f ms", d.net.estimator_base_delay_ms);
        stat_row(s, live, "当前 transit", "%.2f ms", d.net.estimator_transit_ms);
        stat_row(s, live, "transit 电平", "%.2f ms", d.net.estimator_transit_level_ms);
        stat_row(s, live, "乱序包", "%llu",
            static_cast<unsigned long long>(d.net.estimator_reordered_packets));
        stat_row(s, live, "迟到包", "%llu",
            static_cast<unsigned long long>(d.net.estimator_late_packets));
        stat_row(s, live, "重复包", "%llu",
            static_cast<unsigned long long>(d.net.estimator_duplicate_packets));
        stat_row(s, live, "序列缺口事件", "%llu",
            static_cast<unsigned long long>(d.net.rx_audio_sequence_gap_events));
        stat_row(s, live, "缺失帧", "%llu",
            static_cast<unsigned long long>(d.net.rx_audio_sequence_missing_frames));
        stat_row(s, live, "已收音频帧", "%llu",
            static_cast<unsigned long long>(d.net.audio_frames_accepted));
        stat_row(s, live, "畸形包", "%llu",
            static_cast<unsigned long long>(d.net.malformed_datagrams));
        ImGui::EndTable();
    }

    // ---- UDP 传输 ----
    ImGui::SeparatorText("传输");
    if (begin_stat_table("##stats_transport")) {
        stat_row(s, live, "收包", "%llu",
            static_cast<unsigned long long>(d.net.transport.rx_packets));
        stat_row_text(s, "收字节", live ? format_bytes(d.net.transport.rx_bytes) : std::string("-"));
        stat_row(s, live, "收包错误", "%llu",
            static_cast<unsigned long long>(d.net.transport.rx_errors));
        stat_row(s, live, "对端不可达", "%llu",
            static_cast<unsigned long long>(d.net.transport.rx_unreachable));
        stat_row(s, live, "发包", "%llu",
            static_cast<unsigned long long>(d.net.transport.tx_packets));
        stat_row_text(s, "发字节", live ? format_bytes(d.net.transport.tx_bytes) : std::string("-"));
        stat_row(s, live, "发送丢弃", "%llu",
            static_cast<unsigned long long>(d.net.transport.tx_dropped));
        ImGui::EndTable();
    }

    // ---- heartbeat（建连握手 + 稳态探活）----
    ImGui::SeparatorText("心跳");
    if (begin_stat_table("##stats_hb")) {
        stat_row(s, live, "ACK 总数", "%llu",
            static_cast<unsigned long long>(d.net.heartbeat_ack_count));
        stat_row(s, live, "连续未回应", "%u", d.net.heartbeat_ack_misses);
        stat_row(s, live, "距上次 ACK", "%lld ms / %lld ms",
            static_cast<long long>(d.net.heartbeat_ack_age_ms),
            static_cast<long long>(aqua::config::HEARTBEAT_INTERVAL.count()));
        ImGui::EndTable();
    }

    // ---- 抖动缓冲 ----
    ImGui::SeparatorText("抖动缓冲");
    if (begin_stat_table("##stats_jb")) {
        const auto& jb = d.jitter_buffer;
        stat_row(s, live, "水位", "%.1f %%", jb.water_level * 100.0);
        stat_row(s, live, "占用 / 容量", "%u / %u 槽", jb.used_slots, jb.capacity_slots);
        stat_row(s, live, "领先", "%.1f ms（%u 槽）", jb.lead_ms, jb.lead_slots);
        stat_row(s, live, "目标", "%.1f ms（%u 槽）", jb.target_ms, jb.target_slots);
        stat_row(s, live, "目标区间", "%u – %u 槽", d.jitter_control.min_slots,
            d.jitter_control.max_slots);
        stat_row(s, live, "播放序列", "%llu",
            static_cast<unsigned long long>(jb.play_sequence));
        stat_row(s, live, "最高收到序列", "%llu",
            static_cast<unsigned long long>(jb.highest_received_sequence));
        stat_row(s, live, "欠载次数", "%llu",
            static_cast<unsigned long long>(jb.underrun_events));
        stat_row(s, live, "欠载帧", "%llu",
            static_cast<unsigned long long>(jb.underrun_frames));
        stat_row(s, live, "欠载占比", "%.2f %%", jb.underrun_ratio * 100.0);
        stat_row(s, live, "掩盖 / 饱和退回", "%llu / %llu 槽",
            static_cast<unsigned long long>(jb.concealed_slots),
            static_cast<unsigned long long>(jb.concealed_saturated_slots));
        stat_row(s, live, "Fill / Drop 占比", "%.2f %% / %.2f %%",
            jb.fill_duty * 100.0, jb.drop_duty * 100.0);
        stat_row(s, live, "重锚次数", "%llu",
            static_cast<unsigned long long>(jb.reanchor_count));
        ImGui::EndTable();
    }

    ImGui::Dummy(ImVec2(0.0f, 4.0f));
    ImGui::TextDisabled("字段口径与 CLI / Android 的诊断一致（同一份 ClientDiagnosticsSnapshot）。");
}

} // namespace aqua::gui
