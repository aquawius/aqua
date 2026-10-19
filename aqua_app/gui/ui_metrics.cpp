#include "ui_panels.h"

#include "ui_demo.h"
#include "ui_state.h"

#include <aqua/audio/audio_format.h>
#include <aqua/audio/buffer/buffer_config.h>
#include <aqua/audio/playback/audio_playback.h>
#include <aqua/net/udp/udp_config.h>

#include <imgui.h>

#include <SDL3/SDL.h>
#include <cstdlib>

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
    { "发送量", "本次会话累计发出的字节数（心跳 + 回执）" },
    { "乱序/迟到", "乱序到达的包 / 到达太晚已被跳过的包（迟到会直接造成掩盖）" },
    { "心跳", "链路存活：心跳 ACK 正常 / 连续未回应的次数。**不显示「距上次 ACK」**——那是"
              "相位量，跟 1 s 的心跳周期比着锯齿跳，放主页只会误导；原始毫秒数在设置→统计" },
    { "播放", "本机播放是否在跑（PlaybackManager 状态）。连接正常但播放停了就听不到声音" },
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

// 演示模式下 14 个指标的拟真值（水面数据 + 链路数据两套公式拼起来）。
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
        case MetricId::TxBytes:
            // 发送侧：客户端只发心跳与回执，量级是 KB。
            std::snprintf(buf, sizeof(buf), "%.2f KB",
                demo.link[2] * 4.0 * (ImGui::GetTime() + 120.0) / 1024.0);
            break;
        case MetricId::ReorderLate:
            std::snprintf(buf, sizeof(buf), "%.0f / %.0f", demo.link[4], demo.link[5]);
            break;
        case MetricId::Heartbeat:
            std::snprintf(buf, sizeof(buf), "%s", "正常");
            break;
        case MetricId::Playback:
            std::snprintf(buf, sizeof(buf), "%s", "运行中");
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
    case MetricId::TxBytes:
        return format_bytes(d.net.transport.tx_bytes);
    case MetricId::ReorderLate:
        std::snprintf(buf, sizeof(buf), "%llu / %llu",
            static_cast<unsigned long long>(d.net.estimator_reordered_packets),
            static_cast<unsigned long long>(d.net.estimator_late_packets));
        break;
    case MetricId::Heartbeat:
        // 显示**状态**而不是"距上次 ACK"：后者是相位量（心跳 1 s 一次、快照 500ms
        // 一采），天生在 0..1000ms 之间锯齿跳，放主页只会被当成"网络在抖"。
        // 原始毫秒数与阈值在设置→统计里。
        if (d.net.heartbeat_failed) {
            std::snprintf(buf, sizeof(buf), "%s", "中断");
        } else if (d.net.heartbeat_ack_misses > 0) {
            std::snprintf(buf, sizeof(buf), "缺 %u 次", d.net.heartbeat_ack_misses);
        } else {
            std::snprintf(buf, sizeof(buf), "%s", "正常");
        }
        break;
    case MetricId::Playback:
        std::snprintf(buf, sizeof(buf), "%s", d.playback_running ? "运行中" : "未运行");
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
    case MetricId::Heartbeat:
        if (d.net.heartbeat_failed) {
            return red;
        }
        return (d.net.heartbeat_ack_misses > 0) ? yellow : normal;
    case MetricId::Playback:
        return d.playback_running ? normal : yellow;
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

// 编码枚举 → 显示名（与 client_session 的 encoding_label 同口径：PCM_F32LE →
// "PCM F32LE"）。远端格式分节用。
std::string remote_encoding_name(int enc)
{
    switch (static_cast<aqua::audio::AudioEncoding>(enc)) {
    case aqua::audio::AudioEncoding::PCM_S16LE: return "PCM S16LE";
    case aqua::audio::AudioEncoding::PCM_S32LE: return "PCM S32LE";
    case aqua::audio::AudioEncoding::PCM_F32LE: return "PCM F32LE";
    case aqua::audio::AudioEncoding::PCM_S24LE: return "PCM S24LE";
    case aqua::audio::AudioEncoding::PCM_U8:    return "PCM U8";
    default:                                    return "-";
    }
}

// printf → std::string：统计行里"先判断再格式化"的值（窗口未满 / 固定模式…）
// 没法走 va 版 stat_row，统一走这里。
std::string text_of(const char* fmt, ...)
{
    char buf[96];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    return buf;
}

// TargetController 的本拍收敛路径（jitter_control.path；4 = deadband 已废弃，
// ADR-4：死区恒 0、永不可达，这里不再列它）。
const char* target_path_label(int path)
{
    switch (path) {
    case 1:  return "上升";
    case 2:  return "回落";
    case 3:  return "涨后锁跌";
    case 4:  return "死区（不可达）";
    case 5:  return "无时间基准";
    case 6:  return "风暴冻结";
    default: return "稳态";
    }
}

// 播放流后端（stream.backend = None 表示播放流没起来，后面几行都是占位）。
const char* stream_backend_label(aqua::audio::AudioStreamInfo::Backend backend)
{
    using aqua::audio::AudioStreamInfo;
    switch (backend) {
    case AudioStreamInfo::Backend::AAudio: return "AAudio";
    case AudioStreamInfo::Backend::Wasapi: return "WASAPI";
    case AudioStreamInfo::Backend::None:
    default:                               return "-";
    }
}

// 性能模式（与 AAUDIO_PERFORMANCE_MODE_* 同一取值空间，WASAPI 复用）。
const char* performance_mode_label(std::int32_t mode)
{
    using aqua::audio::AudioStreamInfo;
    switch (mode) {
    case AudioStreamInfo::kPerformanceLowLatency:  return "低延迟";
    case AudioStreamInfo::kPerformancePowerSaving: return "省电";
    case AudioStreamInfo::kPerformanceNone:        return "无";
    default:                                       return "-";
    }
}

} // namespace

// ---- 设置 → 统计 ----
// 完整明细，**分节与连接状态无关**：未连接时每一行都显示 "-"（占位），连上后原地
// 填入本次会话的值；这样可以一眼看出"这一页总共有哪些量"。分节顺序 = 数据流：
//   音频格式（会话契约）→ 实时指标（面板那 14 个数字 + 口径说明）→ 会话 →
//   网络 → 传输 → 心跳 → 抖动缓冲 → 自适应缓冲（决策层）→ 播放输出（本机链路）
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

    // 一句话说明放表格下方（不是每行都有）：解释"这一节是什么口径"，比逐行
    // 复述字段名有用，也不占右侧说明列的位置。
    ImGui::TextDisabled("由服务端在握手时下发，整场不变；改服务器配置后需重新连接。");

    // ---- 输出流：引擎周期与格式协商（core 2026-10-19 第二轮诊断）----
    // 回答"这一拍回调到底多大、谁在重采样"：frames_per_burst 历史上报的是引擎
    // fundamental 而不是选定周期（几何地板因此算错），现在两者都在这里。
    if (live && s.session.diag.stream.backend != audio::AudioStreamInfo::Backend::None) {
        const auto& ex = s.session.diag_ex;
        const auto& si = s.session.diag.stream;
        if (begin_stat_table("##stats_stream_ex")) {
            stat_row_text(s, "引擎周期",
                text_of("%u 帧（默认 %u）", ex.engine_period_frames, ex.engine_period_default_frames));
            if (ex.engine_period_fundamental_frames > 0) {
                // 基本周期 = 选定周期的"粒度单位"（选定周期必是它的整数倍）。
                // 两个都报才解释得了"周期为什么是 336 而最小是 48"。
                stat_row_text(s, "引擎基本周期",
                    text_of("%u 帧（周期是它的整数倍）", ex.engine_period_fundamental_frames));
            }
            if (ex.engine_period_min_frames > 0 || ex.engine_period_max_frames > 0) {
                stat_row_text(s, "周期范围",
                    text_of("%u – %u 帧", ex.engine_period_min_frames, ex.engine_period_max_frames));
            }
            stat_row_text(s, "回调帧数", text_of("%u 帧", ex.callback_frames));
            stat_row_text(s, "引擎重采样", ex.engine_resampling ? "是（OS SRC 在线路里）" : std::string("否"));
            if (ex.mix_sample_rate > 0) {
                stat_row_text(s, "端点混音格式", text_of("%u Hz / %u ch", ex.mix_sample_rate, ex.mix_channels));
            }
            stat_row_text(s, "采样率被调整", ex.sample_rate_adjusted ? "是（系统 SRC）" : std::string("否"));
            stat_row_text(s, "端点缓冲", text_of("%u 帧", si.buffer_capacity_frames));
            ImGui::EndTable();
        }
        ImGui::TextDisabled("引擎周期 = 实际传给音频引擎的每拍帧数；「回调帧数」是几何地板（最低延迟）的输入。");
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
    const aqua::gui::SessionData::DiagEx& ex = s.session.diag_ex;

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
        // 播放状态不在这里重复：它是实时指标表里的「播放」一项。
        ImGui::EndTable();
    }

    ImGui::TextDisabled("会话号与运行日志里每行的 session=0x… 对应，便于两边对照排障。");

    // ---- 远端协商格式（core 2026-10-19）：与"本机播放流"对照着看 ----
    // 回答"服务端给的格式和我实际播的为什么不一样"：本机播放流的采样率可能被
    // 系统 SRC 改过（见上面的「采样率被调整」），远端格式才是服务端的原样。
    if (live) {
        if (ex.remote_sample_rate > 0 && begin_stat_table("##stats_remote")) {
            stat_row_text(s, "采样率", format_sample_rate(ex.remote_sample_rate));
            stat_row_text(s, "声道", format_channels(ex.remote_channels));
            stat_row_text(s, "编码", remote_encoding_name(ex.remote_encoding));
            if (ex.remote_bitrate_kbps > 0) {
                stat_row_text(s, "码率", text_of("%u kbps", ex.remote_bitrate_kbps));
            }
            stat_row_text(s, "每包帧数", text_of("%u 帧", ex.remote_frame_count));
            ImGui::EndTable();
        }
        ImGui::TextDisabled("服务端编码后的原始格式；上面「音频格式」是本机播放流的实际参数。");
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
        // ---- 第二轮诊断扩充（core 2026-10-19）----
        stat_row(s, live, "连续收包错误", "%u", ex.rx_consecutive_errors);
        if (ex.rx_last_error_code != 0) {
            stat_row_text(s, "最近错误码", text_of("%d", ex.rx_last_error_code));
        }
        stat_row(s, live, "观测包总数", "%llu",
            static_cast<unsigned long long>(ex.estimator_packets));
        stat_row(s, live, "观测缺口事件", "%llu",
            static_cast<unsigned long long>(ex.estimator_gap_events));
        stat_row(s, live, "观测缺失包", "%llu",
            static_cast<unsigned long long>(ex.estimator_missing_packets));
        stat_row_text(s, "底噪已确立", ex.estimator_base_set ? "是" : std::string("否（base_delay=0 是未建立）"));
        stat_row_text(s, "数据面族",
            ex.udp_family == 4 ? std::string("IPv4")
            : (ex.udp_family == 6 ? std::string("IPv6") : std::string("-")));
        stat_row_text(s, "数据面地址来源",
            ex.udp_address_from_sentinel ? std::string("服务端通告通配，复用控制面地址")
                                         : std::string("服务端通告的具体地址"));
        // 实际对端（观测值）：与上面"通告的地址"可能不同（NAT / 多网卡 / 通配地址）。
        // 空 = 还没学到（还没收到过对端回包）。
        stat_row_text(s, "实际对端",
            ex.learned_peer_endpoint.empty() ? std::string("-（尚未学到）")
                                             : ex.learned_peer_endpoint);
        ImGui::EndTable();
    }

    ImGui::TextDisabled("JitterEstimator 只观测不干预：这些值用于诊断与 target 参考，不直接改水位。");

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

    ImGui::TextDisabled("UDP 数据面的累计值；「对端不可达」持续增长通常是端口或 NAT 配置问题。");

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

    ImGui::TextDisabled("服务端每秒回一次 ACK；连续未回应达到阈值即判会话中断并触发重连。");

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
        // ---- 第二轮诊断扩充（core 2026-10-19）----
        stat_row(s, live, "最旧已收序列", "%llu",
            static_cast<unsigned long long>(ex.oldest_received_sequence));
        if (live && jb.highest_received_sequence >= ex.oldest_received_sequence
            && jb.highest_received_sequence > 0) {
            // occupied span = highest - oldest + 1：真实占用。lead 基于播放头，
            // 两者差距就是"播放头后面拖着的空洞"。
            const auto span = jb.highest_received_sequence - ex.oldest_received_sequence + 1;
            stat_row_text(s, "占用跨度", text_of("%llu 槽", static_cast<unsigned long long>(span)));
        }
        stat_row_text(s, "掩盖 / 拼接开关",
            text_of("%s / %s", ex.conceal_enabled ? "掩盖开" : "掩盖关",
                ex.splice_enabled ? "拼接 crossfade 开" : "拼接硬切"));
        stat_row(s, live, "掩盖上限", "%u 槽", ex.conceal_max_slots);
        stat_row(s, live, "启动预填充", "%u 槽", ex.startup_slots);
        stat_row(s, live, "启动初始 target", "%u 槽", ex.initial_target_slots);
        ImGui::EndTable();
    }

    ImGui::TextDisabled("执行层：缓冲里存了多少音频、领先目标多久、欠载与掩盖各发生多少次。"
                        "「掩盖 / 拼接开关」解释计数为 0 是没触发还是功能没开。");

    // ---- 自适应缓冲（TargetController 决策层：为什么 target 是现在这样）----
    // 上面「抖动缓冲」讲的是执行层的**结果**，这一节讲**为什么**：期望值与实际值
    // 不等 = 被下限抬起 / 上限夹住 / 涨后锁跌 / 风暴冻结按住，具体看「限位」与
    // 「收敛路径」两行。固定模式（--jb-fixed-target）下 controller 不创建，
    // 决策层字段全 0，与 Android 的「自适应缓冲」卡同一口径。
    ImGui::SeparatorText("自适应缓冲");
    if (begin_stat_table("##stats_ctl")) {
        const auto& jc = d.jitter_control;
        stat_row_text(s, "模式", live ? (jc.adaptive ? std::string("自适应") : std::string("固定"))
                                      : std::string("-"));
        stat_row(s, live, "期望 target", "%u 槽", jc.desired_slots);
        stat_row(s, live, "可用区间", "%u – %u 槽", jc.min_slots, jc.max_slots);
        std::string bound = "-";
        if (live) {
            bound = jc.floor_bound ? "下限抬起" : (jc.cap_bound ? "上限夹住" : "—");
        }
        stat_row_text(s, "限位", bound);
        stat_row_text(s, "余量来源",
            live ? (jc.margin_source == 1 ? std::string("断流峰值") : std::string("尾部分位"))
                 : std::string("-"));
        stat_row_text(s, "收敛路径", live ? std::string(target_path_label(jc.path))
                                          : std::string("-"));
        stat_row(s, live, "欠载补偿", "+%.1f 槽", jc.underrun_penalty);
        stat_row(s, live, "涨后锁跌剩余", "%.0f ms", jc.dwell_remaining_ms);
        stat_row(s, live, "跌侧限速额度", "%.1f 槽", jc.fall_room_slots);
        stat_row(s, live, "几何地板", "%u 槽", jc.geometric_floor_slots);
        // 尾部 P99 < 0 = 窗口未满（样本 < JB_TAIL_MIN_SAMPLES，直方图还没成型）。
        stat_row_text(s, "尾部 P99",
            live ? (jc.tail_p99_ms < 0.0 ? std::string("窗口未满")
                                        : text_of("%.1f ms", jc.tail_p99_ms))
                : std::string("-"));
        stat_row_text(s, "尾部样本",
            live ? text_of("%llu / %u（满窗）", static_cast<unsigned long long>(jc.tail_samples),
                       aqua::config::JB_TAIL_WINDOW_PACKETS)
                 : std::string("-"));
        stat_row(s, live, "抖动余量", "%.1f 槽", jc.tail_margin_slots);
        stat_row(s, live, "断流事件", "%llu", static_cast<unsigned long long>(jc.stall_events));
        stat_row(s, live, "断流峰值", "%.1f ms", jc.stall_peak_ms);
        stat_row(s, live, "最近断流间隔", "%.1f ms", jc.last_stall_gap_ms);
        stat_row(s, live, "包间隔", "%.2f ms", jc.arrival_interval_ms);
        stat_row(s, live, "告警带", "%u – %u 槽", jc.band_warning_low, jc.band_warning_high);
        stat_row(s, live, "正常带", "%u – %u 槽", jc.band_normal_low, jc.band_normal_high);
        stat_row(s, live, "当前掩盖", "%u 槽", jc.conceal_run_slots);
        stat_row(s, live, "当前缺帧", "%u 槽", jc.underrun_run_slots);
        // ---- 第二轮诊断扩充（core 2026-10-19）：拆解"target 为什么是这个值" ----
        stat_row(s, live, "生效下限", "%u 槽", ex.effective_min_slots);
        stat_row_text(s, "风暴保持", ex.in_storm ? "是（目标被冻住）" : std::string("否"));
        stat_row(s, live, "峰值项给量", "%.2f 槽（上限 %.2f）",
            ex.last_stall_margin_slots, ex.stall_peak_cap_slots);
        stat_row(s, live, "跌侧限速", "%.1f 槽/秒", ex.fall_rate_slots_per_sec);
        ImGui::EndTable();
    }

    ImGui::TextDisabled("决策层：target 为什么是当前值——被下限抬起、上限夹住，还是正在锁跌。");

    // ---- 播放输出（本机播放链路：拉了多少、吐了什么、流开在什么参数上）----
    // "静音帧"是 JB 为修正时间轴（预滚 / 低水位 Hold）主动吐的静音，与上面的
    // 欠载 / 掩盖（没数据可用）是两回事；流参数回答"WASAPI 没 callback，还是
    // callback 了但 JB 没数据"这个二分问题。
    ImGui::SeparatorText("播放输出");
    if (begin_stat_table("##stats_out")) {
        const auto& pb = d.playback;
        const auto& jb = d.jitter_buffer;
        const auto& info = d.stream;
        const bool has_stream = live
            && info.backend != aqua::audio::AudioStreamInfo::Backend::None;
        stat_row(s, live, "拉取调用", "%llu", static_cast<unsigned long long>(pb.pull_calls));
        stat_row(s, live, "输出帧", "%llu", static_cast<unsigned long long>(pb.pull_frames));
        stat_row(s, live, "静音帧", "%llu",
            static_cast<unsigned long long>(pb.pull_silence_frames));
        stat_row_text(s, "静音占比",
            live ? text_of("%.3f %%", pb.pull_frames > 0
                       ? static_cast<double>(pb.pull_silence_frames) * 100.0
                           / static_cast<double>(pb.pull_frames)
                       : 0.0)
                 : std::string("-"));
        stat_row(s, live, "当前连续静音", "%llu 帧",
            static_cast<unsigned long long>(jb.consecutive_silence_frames));
        stat_row(s, live, "最长静音连续", "%llu 帧",
            static_cast<unsigned long long>(jb.max_silence_run_frames));
        stat_row(s, live, "拼接次数", "%llu", static_cast<unsigned long long>(jb.splice_events));
        stat_row(s, live, "Fill / Drop 次数", "%llu / %llu",
            static_cast<unsigned long long>(jb.fill_episodes),
            static_cast<unsigned long long>(jb.drop_episodes));
        stat_row(s, live, "Fill / Drop 槽数", "%llu / %llu",
            static_cast<unsigned long long>(jb.fill_corrected_slots),
            static_cast<unsigned long long>(jb.drop_skipped_slots));
        stat_row(s, live, "push 接受 / 拒绝", "%llu / %llu",
            static_cast<unsigned long long>(jb.push_accepted),
            static_cast<unsigned long long>(jb.push_rejected));
        stat_row(s, live, "迟到拒收", "%llu",
            static_cast<unsigned long long>(jb.push_rejected_late));
        stat_row_text(s, "流后端", has_stream ? std::string(stream_backend_label(info.backend))
                                              : std::string("-"));
        stat_row_text(s, "流采样率 / 声道",
            has_stream ? text_of("%u Hz / %u", info.sample_rate, info.channels)
                       : std::string("-"));
        stat_row_text(s, "性能模式",
            has_stream ? std::string(performance_mode_label(info.performance_mode))
                       : std::string("-"));
        // Burst：AAudio = 原生 burst；WASAPI = 引擎**基本周期**（fundamental period）。
        // 但部分 WASAPI 驱动会报 fundamental = 1（本机 Pro Audio endpoint 实测：
        // 1 帧 @48k = 0.02ms，物理上不可能是真实周期），而真正被采用的 selected
        // period 不在诊断快照里。低于 0.5ms 的上报值一律按"驱动未上报可信值"处理，
        // 原值放进 tooltip——界面上不该出现一个不可能的 "1 帧"。
        const bool burst_sane = has_stream && info.frames_per_burst > 0
            && (info.sample_rate == 0
                || static_cast<std::uint64_t>(info.frames_per_burst) * 2000u >= info.sample_rate);
        stat_row_text(s, "Burst",
            burst_sane ? text_of("%u 帧", info.frames_per_burst) : std::string("-"));
        if (has_stream && !burst_sane && info.frames_per_burst > 0 && ImGui::IsItemHovered()) {
            ImGui::SetTooltip("驱动上报的引擎基本周期 = %u 帧（< 0.5 ms），视作未上报可信值。\n"
                              "WASAPI 下这是 driver 的 fundamental period，不一定是实际采用的周期。",
                info.frames_per_burst);
        }
        stat_row_text(s, "设备缓冲容量",
            (has_stream && info.buffer_capacity_frames > 0)
                ? text_of("%u 帧", info.buffer_capacity_frames) : std::string("-"));
        stat_row(s, live, "后端回调", "%llu", static_cast<unsigned long long>(info.callback_count));
        stat_row(s, live, "端点填充", "%u 帧", info.current_padding_frames);
        // 本机延迟合计 = 抖动缓冲里积压的音频（lead）+ 设备端点里排队的（padding）。
        // 这是"本机能听到的最短延迟"的可测部分：不含网络单程与 DAC 自身缓冲，
        // 所以叫"本机延迟"而不是"端到端"（那需要包内时间戳 + 时钟同步）。
        if (live && info.sample_rate > 0) {
            const double padding_ms = static_cast<double>(info.current_padding_frames) * 1000.0
                / static_cast<double>(info.sample_rate);
            stat_row_text(s, "本机延迟（合计）",
                text_of("%.1f ms = 缓冲 %.1f + 设备 %.1f",
                    d.jitter_buffer.lead_ms + padding_ms, d.jitter_buffer.lead_ms, padding_ms));
        } else {
            stat_row_text(s, "本机延迟（合计）", "-");
        }
        stat_row(s, live, "设备 XRun", "%llu", static_cast<unsigned long long>(info.xrun_count));
        // ---- 第二轮诊断扩充（core 2026-10-19）：WASAPI 的 underrun + 事件卫生 ----
        stat_row(s, live, "缓冲满跳过", "%llu",
            static_cast<unsigned long long>(ex.empty_event_skips));
        stat_row(s, live, "上次可写", "%u 帧", ex.catchup_frames);
        stat_row(s, live, "可写峰值", "%u 帧", ex.catchup_frames_max);
        stat_row(s, live, "过期事件丢弃", "%llu",
            static_cast<unsigned long long>(ex.stale_events_dropped));
        // ---- 路由与切换（运行期切换播放设备后，这里能看出实际落在哪）----
        // 「请求」与「实际」在降级 / 兜底 / 自动跟随时会不一样；诊断快照给的是
        // core 的决策结果，UI 只转述。
        stat_row_text(s, "路由模式",
            live ? std::string(aqua::audio::route_intent_owner_label(d.route_mode))
                 : std::string("-"));
        stat_row_text(s, "请求设备",
            live ? (d.requested_device_id.empty() ? std::string("跟随系统")
                                                  : std::string(d.requested_device_id.value()))
                 : std::string("-"));
        stat_row_text(s, "实际输出设备",
            (live && !d.stream.device_id.empty())
                ? std::string(d.stream.device_id.value()) : std::string("-"));
        // 值列装不下 WASAPI 的长 endpoint id（会被裁掉一截）：悬停看全文。
        if (live && !d.stream.device_id.empty() && ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", d.stream.device_id.value().c_str());
        }
        stat_row(s, live, "切换序号", "%u", d.switch_seq);
        stat_row_text(s, "最近切换",
            live ? (d.switch_result.outcome == aqua::audio::SwitchOutcome::None
                       ? std::string("尚无")
                       : text_of("%s（%u ms）",
                           aqua::audio::switch_outcome_name(d.switch_result.outcome),
                           d.switch_result.duration_ms))
                 : std::string("-"));
        ImGui::EndTable();
    }

    ImGui::TextDisabled("本机播放链路；「本机延迟」= 缓冲领先 + 设备排队，不含网络单程。");

    ImGui::Dummy(ImVec2(0.0f, 4.0f));
    ImGui::TextDisabled("字段口径与 CLI / Android 的诊断一致（同一份 ClientDiagnosticsSnapshot）。");

    // TEMP-VERIFY：截图验证下半段用（AQUA_GUI_STATS_SCROLL=<0..1>），验证完删除。
    if (const char* frac_env = SDL_getenv("AQUA_GUI_STATS_SCROLL")) {
        const double frac = std::atof(frac_env);
        ImGui::SetScrollY(static_cast<float>(ImGui::GetScrollMaxY() * (frac > 0.0 ? frac : 1.0)));
    }
}

} // namespace aqua::gui
