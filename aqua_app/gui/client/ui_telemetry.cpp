#include "ui_panels.h"

#include "app_style.h"
#include "ui_demo.h"
#include "ui_state.h"

#include <imgui.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>

// 遥测面板：连接页的下半屏（连接卡片之下，吃满剩余高度）。
//
// 版式（2026-10-18 第五轮）：**结构与会话阶段无关**，没连接时用 "-" / "—" 占位，
// 连上后原地换成数字，几何一点不动；信息按"同一主题放同一张卡"分组。
//
//   内容左缘 x0                                                  内容右缘 x1
//   │ 音频格式 48 kHz · 立体声 · PCM F32LE · 1536 kbps · …  [统计…] │
//   │ ───────────────────────────────────────────────────────── │
//   │ ┌ 缓冲 ┄┄ 警戒 12 % ──────────┐ ┌ 链路 ─────────────────┐ │
//   │ │ 缓冲水位 9.0 %   占用 8/30 槽 │ │ 网络抖动        0.9 ms │ │
//   │ │ 领先 29.2 ms    目标 25.5 ms │ │ 缺口率         0.00 % │ │
//   │ │ 欠载 0.00 %   重锚/掩盖 0/0  │ │ 到达速率     270 包/秒 │ │
//   │ │ ┌──────────────────────────┐ │ │ 接收量       12.34 MB │ │
//   │ │ │ 100 ├──────────────────  │ │ │ 乱序/迟到      0 / 0  │ │
//   │ │ │  50 ├──────────────────  │ │ │ ACK 延迟  499/1000 ms │ │
//   │ │ │   0 ├──────────────────  │ │ └───────────────────────┘ │
//   │ │ │ -60 s              现在  │ │                          │
//   │ │ └──────────────────────────┘ │                          │
//   │ └──────────────────────────────┘                          │
//
// 为什么这样分组：缓冲侧的量（水位 / 占用 / 领先 / 目标 / 欠载 / 重锚掩盖）与它们
// 的走势图放同一张卡；链路侧的量（抖动 / 缺口 / 到达速率 / 接收量 / 乱序迟到 /
// ACK）放另一张。之前是"6 个指标一排 + 另一张卡里又放缓冲占用"，同一个量（水位 /
// 占用槽）被拆到两处，看着像两套口径（用户 2026-10-18 反馈"相关的东西要放到同一个
// 位置"）。现在两张卡都从 MetricId 单一数据源取数，标签 / 口径说明 / 健康色一处定义。
//
// 刻意**不**在这里显示「已连接 / 会话号 / 运行时长」：那是"现在正在做什么"，归底部
// 状态栏。音频格式反过来——它是本次会话的固定事实，留在面板第一行。

namespace aqua::gui {
namespace {

// 60 秒历史：500ms 一拍 = 120 点。会话层的发布节拍正是 500ms。
constexpr int kTrendPoints = 120;

// 低水位警戒线：水位掉到这里基本就是欠载前兆。画成虚线，让"数字变小"
// 变成"图形贴近底边"。
constexpr float kLowWaterMark = 0.12f;

// 面板派生缓存（丢了只是重画，不属于 UiState）。
struct TelemetryState {
    float water[kTrendPoints] = {};
    int count = 0; // 已采样的点数（≤ kTrendPoints）
    int head = 0;  // 下一个写入位置
    std::uint64_t last_seq = 0;  // 已消费的快照序号（去重用）
    double last_sample = -1.0e9; // 演示模式的采样节拍
    UiPhase last_phase = UiPhase::Idle;
};

TelemetryState& telemetry_state()
{
    static TelemetryState state;
    return state;
}

void push_water(TelemetryState& v, double level)
{
    v.water[v.head] = static_cast<float>(std::clamp(level, 0.0, 1.0));
    v.head = (v.head + 1) % kTrendPoints;
    if (v.count < kTrendPoints) {
        ++v.count;
    }
}

// 第 i 个点（0 = 最旧，count-1 = 最新）。
float water_at(const TelemetryState& v, int i)
{
    const int start = (v.head - v.count + kTrendPoints) % kTrendPoints;
    return v.water[(start + i) % kTrendPoints];
}

// 一条 1px 分节线（跨满内容宽度）+ 上下各一点呼吸量。
// 不用 ImGui::Separator：它的留白来自 ItemSpacing，面板里几条分节的间距要一致就
// 得自己定。整条只提交**一个** item（Dummy），高度 = 1px + 上下 pad，行距按 ImGui
// 的惯例另算——预算里漏算行距会把内容挤出面板（上一版实测过）。
float rule_item_height(float pad)
{
    return 1.0f + pad * 2.0f;
}

void section_rule(float pad)
{
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x;
    ImGui::GetWindowDrawList()->AddLine(ImVec2(p.x, p.y + pad + 0.5f),
        ImVec2(p.x + w, p.y + pad + 0.5f), ImGui::GetColorU32(ImGuiCol_Separator), 1.0f);
    ImGui::Dummy(ImVec2(w, rule_item_height(pad)));
}

void dashed_hline(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 col, float dash, float gap)
{
    for (float x = a.x; x < b.x; x += dash + gap) {
        dl->AddLine(ImVec2(x, a.y), ImVec2(std::min(x + dash, b.x), a.y), col, 1.0f);
    }
}

// ---- 卡片的公共底：圆角底 + 1px 边（圆角取设置的「圆角」值，与 tab / 输入框同源）----
void begin_card(ImVec2 p0, ImVec2 p1, float rounding)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, p1, ImGui::GetColorU32(ImGuiCol_FrameBg, 0.30f), rounding);
    dl->AddRect(p0, p1, ImGui::GetColorU32(ImGuiCol_Border), rounding);
}

// ---- 缓冲卡（左）----
// 卡内自上而下：标题行（"缓冲" + 右侧警戒线图例）→ 6 个指标（3 行 × 2 组）→
// 水位曲线（卡内纵轴刻度 + 4×4 方格纸 + 横轴说明）。曲线与数字同卡 = 同一件事的
// 两个视角（此刻是多少 / 最近 60 秒怎么走）。
void draw_buffer_card(const TelemetryState& v, UiState& s, ImVec2 p0, ImVec2 size, float k,
    const char* empty_hint)
{
    if (size.x < 160.0f * k || size.y < 80.0f * k) {
        return;
    }
    const ImGuiStyle& st = ImGui::GetStyle();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 b1(p0.x + size.x, p0.y + size.y);
    const float text_h = ImGui::GetTextLineHeight();
    const float pad = 9.0f * k;
    const float inner_l = p0.x + 1.0f + pad;
    const float inner_r = b1.x - 1.0f - pad;
    const float inner_t = p0.y + 1.0f + pad;
    const float inner_w = inner_r - inner_l;

    begin_card(p0, b1, s.rounding);

    // ---- 标题行：左"缓冲"，右警戒线图例（虚线色标 + 文字，红色 = 与水里的虚线同色）
    const float title_y = inner_t;
    dl->AddText(ImVec2(inner_l, title_y), ImGui::GetColorU32(ImGuiCol_Text), "缓冲");
    {
        ImVec4 warn = semantic_error();
        warn.w = 0.85f;
        const char* legend = "警戒 12 %";
        const float legend_w = ImGui::CalcTextSize(legend).x;
        const float swatch_w = 14.0f * k;
        const float right = inner_r;
        const float sw_x = right - legend_w - st.ItemInnerSpacing.x - swatch_w;
        if (sw_x > inner_l + 60.0f * k) {
            dashed_hline(dl, ImVec2(sw_x, title_y + text_h * 0.5f),
                ImVec2(sw_x + swatch_w, title_y + text_h * 0.5f),
                ImGui::GetColorU32(ImVec4(warn.x, warn.y, warn.z, 0.55f)), 3.0f * k, 3.0f * k);
            dl->AddText(ImVec2(right - legend_w, title_y), ImGui::GetColorU32(warn), legend);
        }
    }

    // ---- 6 个缓冲指标（3 行 × 2 组）----
    const MetricId buffer_ids[kBufferedMetricCount] = {
        MetricId::WaterPct, MetricId::Slots, MetricId::Lead,
        MetricId::Target, MetricId::Underrun, MetricId::Glitches,
    };
    const float metrics_y = title_y + text_h + 6.0f * k;
    ImGui::SetCursorScreenPos(ImVec2(inner_l, metrics_y));
    draw_metric_grid(s, buffer_ids, kBufferedMetricCount, 2, inner_w);
    const float metrics_h = ImGui::GetTextLineHeightWithSpacing()
        * static_cast<float>((kBufferedMetricCount + 1) / 2);

    // ---- 曲线：占满剩下的高度，横轴说明留一行在卡内底部 ----
    const float cap_y = b1.y - 1.0f - pad - text_h;
    const ImVec2 g0(inner_l + ImGui::CalcTextSize("100").x + st.ItemInnerSpacing.x * 2.0f,
        metrics_y + metrics_h + 4.0f * k);
    const ImVec2 g1(inner_r, cap_y - 3.0f * k);
    const float ph = g1.y - g0.y;
    const float pw = g1.x - g0.x;
    if (pw < 40.0f || ph < 24.0f) {
        return; // 卡片被压得太扁：只画标题与数字，别硬塞一张读不出来的图
    }

    // 网格：横 4 等分、纵 4 等分。
    const ImU32 grid = ImGui::GetColorU32(ImGuiCol_TextDisabled, 0.14f);
    for (int i = 0; i <= 4; ++i) {
        const float y = g1.y - ph * 0.25f * static_cast<float>(i);
        dl->AddLine(ImVec2(g0.x, y), ImVec2(g1.x, y), grid, 1.0f);
    }
    for (int i = 1; i < 4; ++i) {
        const float x = g0.x + pw * 0.25f * static_cast<float>(i);
        dl->AddLine(ImVec2(x, g0.y), ImVec2(x, g1.y), grid, 1.0f);
    }

    // 纵轴刻度：0 贴底、100 贴顶、50 居中（两端对齐而不是都居中，字才不会探出卡外）。
    const ImU32 dim = ImGui::GetColorU32(ImGuiCol_TextDisabled, 0.85f);
    struct Tick {
        float frac;
        const char* text;
    };
    const Tick ticks[3] = { { 1.0f, "100" }, { 0.5f, "50" }, { 0.0f, "0" } };
    for (const Tick& t : ticks) {
        const float tw = ImGui::CalcTextSize(t.text).x;
        float y = g1.y - ph * t.frac - text_h * 0.5f;
        if (t.frac >= 1.0f) {
            y = g0.y;
        } else if (t.frac <= 0.0f) {
            y = g1.y - text_h;
        }
        dl->AddText(ImVec2(g0.x - st.ItemInnerSpacing.x - tw, y), dim, t.text);
    }

    // 欠载警戒线（虚线）。线上不压字：水位正常时它就躺在曲线下方一点，任何行内
    // 标注都会和数据打架；含义放在卡片标题行做图例。
    const float y_low = g1.y - ph * kLowWaterMark;
    const ImVec4 warn_col = semantic_error();
    dashed_hline(dl, ImVec2(g0.x + 1.0f, y_low), ImVec2(g1.x - 1.0f, y_low),
        ImGui::GetColorU32(ImVec4(warn_col.x, warn_col.y, warn_col.z, 0.55f)), 3.0f * k, 3.0f * k);

    // 横轴说明：左端贴绘图区左缘、右端贴绘图区右缘（轴管的是绘图区，不是卡边）。
    const float span_s = static_cast<float>(std::max(v.count - 1, 1)) * 0.5f;
    const float shown_span = (v.count >= 2) ? span_s : 60.0f;
    char span_text[24];
    std::snprintf(span_text, sizeof(span_text),
        (shown_span < 9.95f) ? "-%.1f s" : "-%.0f s", shown_span);
    dl->AddText(ImVec2(g0.x, cap_y), dim, span_text);
    const char* now_text = "现在";
    dl->AddText(ImVec2(g1.x - ImGui::CalcTextSize(now_text).x, cap_y), dim, now_text);

    if (v.count < 2) {
        // 没有数据：网格与警戒线照画（骨架不变），中间给一句阶段提示。
        const ImVec2 ts = ImGui::CalcTextSize(empty_hint);
        dl->AddText(ImVec2(g0.x + (pw - ts.x) * 0.5f, g0.y + (ph - ts.y) * 0.5f),
            ImGui::GetColorU32(ImGuiCol_TextDisabled), empty_hint);
        return;
    }

    // 数据（裁到绘图区内：折线 / 圆点绝不越过绘图区边框）。
    const float step = pw / static_cast<float>(v.count - 1);
    ImVec2 line[kTrendPoints];
    for (int i = 0; i < v.count; ++i) {
        const float x = g1.x - step * static_cast<float>(v.count - 1 - i);
        line[i] = ImVec2(x, g1.y - ph * water_at(v, i));
    }

    const ImVec4 c = water_color(static_cast<double>(water_at(v, v.count - 1)));
    const ImU32 col = ImGui::GetColorU32(c);
    const ImU32 fill = ImGui::GetColorU32(ImVec4(c.x, c.y, c.z, 0.16f));

    dl->PushClipRect(g0, g1, true);
    // 曲线下方填充。**采样点太少时不填**：刚连上的头几秒只有 2~3 个点，折线被拉满
    // 整幅宽度，一填就成了从底边斜上去的一大块三角形（用户 2026-10-18 反馈"图表上
    // 有个像三角形的东西"）。点少时只画折线 + 采样点，形状一眼就能看出"才采了几拍"。
    //
    // 填充方式：**每两个相邻采样点一个四边形**，不用 AddConcavePolyFilled。
    // 凹多边形耳切在两种真实数据下会画出区域外的大三角形（用户 2026-10-19 截图：
    // "从左上斜到右下的一块"）：①首点水位 = 0 时 area[0] 与 line[0] 退化成同一个点；
    // ②水位平稳时整段共线。四边形填充是 O(N)、不依赖三角化，任何数据都只会填在
    // 折线与底边之间。
    if (v.count >= 12) {
        const ImDrawListFlags saved_flags = dl->Flags;
        dl->Flags &= ~ImDrawListFlags_AntiAliasedFill;
        for (int i = 0; i + 1 < v.count; ++i) {
            const ImVec2& a = line[i];
            const ImVec2& b = line[i + 1];
            dl->AddQuadFilled(a, b, ImVec2(b.x, g1.y), ImVec2(a.x, g1.y), fill);
        }
        dl->Flags = saved_flags;
    }
    dl->AddPolyline(line, v.count, col, ImDrawFlags_None, 1.5f);
    // 采样点少（< 12 个 = 刚连上的头 6 秒）时把每个采样点出来：这时的折线被拉满
    // 整幅宽度，光看线分不清"只有两个采样"还是"一路平稳"。
    if (v.count <= 12) {
        for (int i = 0; i < v.count; ++i) {
            dl->AddCircleFilled(line[i], 2.5f * k, col, 8);
        }
    }
    dl->AddCircleFilled(line[v.count - 1], 3.0f * k, col); // 末端圆点 = "现在这一拍"
    dl->PopClipRect();
}

// ---- 链路卡（右）----
// 曲线右边那张 8 行的「标签 ……… 数值」表。它答的是曲线答不了的问题：**数据还
// 在不在流、网络有没有在拖后腿、心跳还活着吗**。
//
// 对齐规则（与左侧缓冲卡同一套）：卡片上下缘同高；行数固定，行高 = 内高 / 行数
// （行与行严格等距，水平细线就是行边界）；标签左对齐到内左缘、数值右对齐到内右缘，
// 两列各成一条竖线；数值用等宽字（位数变化不推挤标签）。
void draw_link_card(const UiState& s, ImVec2 p0, ImVec2 size, float k)
{
    if (size.x < 120.0f * k || size.y < 80.0f * k) {
        return;
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 b1(p0.x + size.x, p0.y + size.y);
    const float text_h = ImGui::GetTextLineHeight();
    const float pad = 9.0f * k;
    const float inner_l = p0.x + 1.0f + pad;
    const float inner_r = b1.x - 1.0f - pad;
    const float inner_t = p0.y + 1.0f + pad;
    const float inner_b = b1.y - 1.0f - pad;

    begin_card(p0, b1, s.rounding);

    // 标题行："链路"。与左卡标题同一基线、同一颜色。
    // 注：卡片标题叫"链路"，但内容不限于链路——心跳存活、播放状态、收发量都属于
    // "不看曲线也要随时可见"的东西（用户 2026-10-18：这张卡是面板的第二个信息位）。
    dl->AddText(ImVec2(inner_l, inner_t), ImGui::GetColorU32(ImGuiCol_Text), "链路");

    // 行序 = 重要度：抖动 / 缺口率 / 到达速率 / 心跳 / 播放状态是判断"链路还在不在、
    // 有没有拖后腿"的必需品；乱序·迟到与收发量是明细（统计页都有完整一份）。
    // 卡片矮时从尾部省行（见下面的 avail_h 计算），这组顺序就是"先省谁"的顺序。
    const MetricId link_ids[] = {
        MetricId::Jitter, MetricId::Gap, MetricId::RxRate, MetricId::Heartbeat,
        MetricId::Playback, MetricId::ReorderLate, MetricId::RxBytes, MetricId::TxBytes,
    };
    static_assert(static_cast<int>(sizeof(link_ids) / sizeof(link_ids[0])) == kLinkMetricCount,
        "右侧卡的指标数必须与 MetricId 链路组一致");
    constexpr int kLinkRows = static_cast<int>(sizeof(link_ids) / sizeof(link_ids[0]));

    const float rows_top = inner_t + text_h + 6.0f * k;
    const float avail_h = inner_b - rows_top;
    int rows = kLinkRows;
    float row_h = avail_h / static_cast<float>(rows);
    // 放不下就让掉末尾几行（最少留 4 行）：宁可少显示两行明细，也别整张卡只剩标题
    // ——窗口 760 高时实测过：连接卡片多两行输入框，这张卡就整个空掉了。
    while (rows > 4 && row_h < text_h) {
        --rows;
        row_h = avail_h / static_cast<float>(rows);
    }
    if (row_h < text_h) {
        return; // 卡片被压扁：只显示标题
    }

    const ImU32 dim = ImGui::GetColorU32(ImGuiCol_TextDisabled);
    const ImU32 hair = ImGui::GetColorU32(ImGuiCol_TextDisabled, 0.12f);
    const float mono = mono_size(s);
    const float value_dy = mono_baseline_dy(s);

    dl->PushClipRect(ImVec2(p0.x + 1.0f, p0.y + 1.0f), ImVec2(b1.x - 1.0f, b1.y - 1.0f), true);
    for (int i = 0; i < rows; ++i) {
        const float row_top = rows_top + row_h * static_cast<float>(i);
        const float text_y = row_top + (row_h - text_h) * 0.5f;

        // 行边界细线：行高严格等分，线就是网格（第一行上方不画，那是标题行）。
        if (i > 0) {
            dl->AddLine(ImVec2(inner_l, row_top), ImVec2(inner_r, row_top), hair, 1.0f);
        }

        dl->AddText(ImVec2(inner_l, text_y), dim, metric_label(link_ids[i]));

        const std::string text = metric_text(s, link_ids[i]);
        ImGui::PushFont(s.font_mono, mono);
        const float value_w = ImGui::CalcTextSize(text.c_str()).x;
        ImGui::PopFont();
        dl->AddText(s.font_mono, mono, ImVec2(inner_r - value_w, text_y + value_dy),
            ImGui::GetColorU32(metric_color(s, link_ids[i])), text.c_str());

        // 悬停说明（用隐形按钮拿 hover：Dummy 不一定进 hover 名单，按钮是稳的）。
        ImGui::SetCursorScreenPos(ImVec2(inner_l, row_top));
        ImGui::PushID(i);
        ImGui::InvisibleButton("##link_row", ImVec2(std::max(inner_r - inner_l, 1.0f), row_h));
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", metric_desc(link_ids[i]));
        }
        ImGui::PopID();
    }
    dl->PopClipRect();
}

// 链路卡宽度：内边距 + 最长标签（实测）+ 间隔 + 14 个等宽字符的数值位。
// 数值位按"字符数"留固定宽，不按当帧实测值宽——否则数字一进位整列会左右抖。
float link_card_width(const UiState& s, float k)
{
    const ImGuiStyle& st = ImGui::GetStyle();
    float label_w = 0.0f;
    const MetricId link_ids[] = {
        MetricId::Jitter, MetricId::Gap, MetricId::RxRate, MetricId::Heartbeat,
        MetricId::Playback, MetricId::ReorderLate, MetricId::RxBytes, MetricId::TxBytes,
    };
    for (MetricId id : link_ids) {
        label_w = std::max(label_w, ImGui::CalcTextSize(metric_label(id)).x);
    }
    ImGui::PushFont(s.font_mono, mono_size(s));
    const float digit_w = ImGui::CalcTextSize("0").x;
    ImGui::PopFont();
    return 2.0f + 2.0f * 9.0f * k + label_w + st.ItemInnerSpacing.x * 2.0f + digit_w * 14.0f;
}

// 窄窗口 / 大字号时的紧凑写法：只留"是什么格式"，帧长与码率让给宽度。
// 数据来自会话层拆好的字段（不反解析那句摘要）；演示模式用自己那套。
std::string compact_format(const UiState& s)
{
    int sr = s.session.sample_rate;
    int ch = s.session.channels;
    int kbps = s.session.bitrate_kbps;
    std::string encoding = s.session.encoding;
    if (s.demo) {
        const DemoFormat demo = demo_format();
        sr = demo.sample_rate;
        ch = demo.channels;
        kbps = demo.bitrate_kbps;
        encoding = demo.encoding;
    }
    char buf[96];
    char rate[32];
    if (sr > 0 && sr % 1000 == 0) {
        std::snprintf(rate, sizeof(rate), "%d kHz", sr / 1000);
    } else if (sr > 0) {
        std::snprintf(rate, sizeof(rate), "%.1f kHz", static_cast<double>(sr) / 1000.0);
    } else {
        std::snprintf(rate, sizeof(rate), "-");
    }
    std::snprintf(buf, sizeof(buf), "%s · %dch%s%s", rate, ch, encoding.empty() ? "" : " · ",
        encoding.empty() ? "" : encoding.c_str());
    std::string text = buf;
    if (kbps > 0) {
        std::snprintf(buf, sizeof(buf), " · %d kbps", kbps);
        text += buf;
    }
    return text;
}

// ---- 音频格式行（面板第一行；会话建立时谈定的格式）----
// 内容是本次会话的固定事实（采样率 · 声道 · 编码 · 码率 · 帧长），连上就不变。
// 照 Android 的排法放在最前面：它是"这场会话在传什么"，比实时数字更早被需要。
// **位置和高度不随阶段变化**：没连上时值是 "—"，连上后原地换成真正的格式串。
// 演示模式没有真实格式，给一套自洽的拟真值。
// 行右端放「统计…」入口：它是这块面板的"详情"，与内容同一行、右缘贴齐。
// 按钮样式与 server 运行信息卡的「统计…」一致（用户 2026-11-06：client 这边
// 不要超链接，改成和 server 一样的按钮）。按钮比文字高约 8px，整行会高一点：
// 卡片高度是从光标位置算出来的（见 draw_telemetry_panel 的 row_at），不用额外补偿。
// 宽度不够时先退成紧凑写法，再不够才裁切——宁可少写帧长，也不要把入口挤没。
void draw_format_row(UiState& s)
{
    const std::string full = s.demo ? std::string(demo_format().summary) : s.session.audio_format;

    // 行内既有文字又有按钮：文字按 frame 高度居中，否则会比按钮高一截（贴顶）。
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("音频格式");
    const ImGuiStyle& st = ImGui::GetStyle();
    // SameLine 收窗口内坐标：标签右缘 = 上一件 item 的**屏幕右缘** - 窗口原点。
    // 不能用 GetCursorPosX()——提交完文本后光标已经在新一行的行首了。
    const float value_x = ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x
        + 14.0f; // 标签与格式串之间留 14px：贴太近会读成"音频格式48 kHz…"（用户 2026-10-19）

    const char* entry = "统计…";
    const float entry_w = ImGui::CalcTextSize(entry).x + st.FramePadding.x * 2.0f;
    const float entry_x = ImGui::GetWindowContentRegionMax().x - entry_w;
    const float avail = entry_x - value_x - st.ItemSpacing.x * 2.0f;

    const auto mono_w = [&](const std::string& text) {
        ImGui::PushFont(s.font_mono, mono_size(s));
        const float w = ImGui::CalcTextSize(text.c_str()).x;
        ImGui::PopFont();
        return w;
    };

    std::string text = full;
    if (full.empty()) {
        text.clear();
    } else if (mono_w(full) > avail) {
        const std::string compact = compact_format(s);
        text = (mono_w(compact) <= avail) ? compact : full; // 都不行就裁切全量
    }

    ImGui::SameLine(value_x);
    if (text.empty()) {
        // 占位也是普通字体，直接顶对齐（不能加基线补偿，否则破折号会掉下去）。
        ImGui::TextDisabled("—");
    } else {
        // 等宽数值比标签字号小，顶对齐会浮高几像素——补一个基线差（见 ui_panels.h）。
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + mono_baseline_dy(s));
        const bool clip_value = mono_w(text) > avail;
        if (clip_value) {
            // AlignTextToFramePadding 会把这一行的文字整体下移 FramePadding.y
            // （TextEx 的绘制位置 = CursorPos + CurrLineTextBaseOffset），裁切矩形
            // 要跟着一起下移，否则会切掉字底。
            const float top = ImGui::GetCursorScreenPos().y + st.FramePadding.y;
            const float right = ImGui::GetWindowPos().x + entry_x - st.ItemSpacing.x;
            ImGui::PushClipRect(ImVec2(0.0f, top - 2.0f),
                ImVec2(right, top + ImGui::GetTextLineHeight() + 2.0f), true);
        }
        ImGui::PushFont(s.font_mono, mono_size(s));
        ImGui::TextUnformatted(text.c_str());
        ImGui::PopFont();
        if (clip_value) {
            ImGui::PopClipRect();
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("音频格式：采样率 · 声道 · 编码 · 码率 · 帧长\n%s\n"
                              "由服务器在连接应答里通告，本次会话内不变。",
                full.c_str());
        }
    }

    ImGui::SameLine(entry_x);
    if (ImGui::Button(entry)) {
        s.settings_tab = kSettingsStats;
        s.show_settings = true;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("打开设置 → 统计（全部指标口径与 core 诊断明细）");
    }
}

} // namespace

void draw_telemetry_panel(UiState& s)
{
    TelemetryState& view = telemetry_state();
    const bool connected = (s.phase == UiPhase::Connected);

    // 阶段迁移：**离开 Connected 也要清**——曲线是"这一刻之前 60 秒"的证据，断开后
    // 它不代表任何东西，留着就和旁边的 "-" 占位互相矛盾（用户 2026-10-18 反馈
    // 音频格式断开不清空，同理）。
    // 进入 Connected 视为"新会话"：曲线从头开始比接着断线前的画更诚实，演示模式
    // 再补一段拟真历史（真实会话没有这段历史，不能编）。
    if (s.phase != view.last_phase) {
        view.count = 0;
        view.head = 0;
        view.last_seq = 0;
        view.last_sample = -1.0e9;
        if (connected && s.demo) {
            const double now = ImGui::GetTime();
            for (int i = kTrendPoints - 1; i >= 0; --i) {
                double d[6];
                fill_demo_tiles_at(now - static_cast<double>(i) * 0.5, d);
                push_water(view, d[0]);
            }
        }
        view.last_phase = s.phase;
    }

    // 采样：真实会话只在"快照序号变了"时采一个点（会话层 500ms 发布一拍）；
    // 演示模式没有序号，按 500ms 节拍自己采。
    if (connected) {
        bool fresh = false;
        if (s.demo) {
            const double now = ImGui::GetTime();
            if (now - view.last_sample >= 0.5) {
                view.last_sample = now;
                fresh = true;
            }
        } else if (s.session.live && s.session.snapshot_seq != view.last_seq) {
            // 只在**拿到诊断数据之后**才采样：publish(Connected) 也会自增 snapshot_seq，
            // 那一拍 water_level 还是 0——少了这个门，曲线最左端会多一个假的 0 点。
            view.last_seq = s.session.snapshot_seq;
            fresh = true;
        }
        if (fresh) {
            double level = s.session.water_level;
            if (s.demo) {
                double d[6];
                fill_demo_tiles(d);
                level = d[0];
            }
            push_water(view, level);
        }
    }

    // 不开 NoScrollbar：窗口被拖得很矮时两张卡有下限，内容会超出面板高度，宁可让
    // ImGui 出一条滚动条，也别把内容永久裁掉。
    if (!ImGui::BeginChild("##telemetry", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders)) {
        // 没有剩余高度（窗口很矮）：跳过内容，但 EndChild 必须照调——BeginChild
        // 已经把子窗口压栈了。
        ImGui::EndChild();
        return;
    }

    // ---- 骨架：任何阶段都是这段，只有值在 "-" / "—" 与真实数字之间切换 ----
    //   音频格式（+ 统计入口）→ 分节线 → 缓冲卡 + 链路卡
    // 刻意**不**按阶段换版式：连接前后面板不许跳形（用户 2026-10-18 明确要求）。
    // 阶段说明（正在连接 / 连接失败）在底部状态栏，不在面板里重复。
    const float k = (s.dpi_scale > 0.0f) ? s.dpi_scale : 1.0f;
    const ImGuiStyle& st = ImGui::GetStyle();
    const float pad = 3.0f * k;

    draw_format_row(s);
    section_rule(pad);

    // ---- 两张并排卡（吃满面板剩余高度）----
    const ImVec2 row_at = ImGui::GetCursorScreenPos();
    const float panel_bottom = ImGui::GetWindowPos().y + ImGui::GetWindowContentRegionMax().y;
    // 下限 150：卡片里有 3 行指标 + 一张图 / 8 行表，再矮就压成一条缝了——宁可口子
    // 不够时让 ImGui 出滚动条（面板没开 NoScrollbar），也别把行挤扁。
    const float body_h = std::max(panel_bottom - row_at.y, 150.0f * k);
    const float total_w = ImGui::GetContentRegionAvail().x;
    const float gap = st.ItemSpacing.x;
    // 右侧表宽按"标签 + 14 个等宽字符"实测；窗口被压窄时让步给左侧（下限 240px）。
    const float side_w = std::min(link_card_width(s, k),
        std::max(total_w - gap - 240.0f * k, 120.0f * k));
    const float buffer_w = std::max(total_w - gap - side_w, 160.0f * k);

    // 曲线没有数据时卡里居中显示的那句话（按阶段给，卡片几何不变）。
    const char* empty_hint = "等待第一帧诊断数据…";
    switch (s.phase) {
    case UiPhase::Idle:          empty_hint = "连接后开始绘制"; break;
    case UiPhase::Connecting:    empty_hint = "正在连接…"; break;
    case UiPhase::Reconnecting:  empty_hint = "正在重连…"; break;
    case UiPhase::Disconnecting: empty_hint = "正在断开…"; break;
    case UiPhase::Failed:        empty_hint = "连接失败，详见运行日志"; break;
    case UiPhase::Connected:
    default:                     break;
    }

    draw_buffer_card(view, s, row_at, ImVec2(buffer_w, body_h), k, empty_hint);
    draw_link_card(s, ImVec2(row_at.x + buffer_w + gap, row_at.y),
        ImVec2(total_w - gap - buffer_w, body_h), k);

    // 两张卡都是自绘（不提交 item），这里补一个占位 item 收尾，让后续 item 落在
    // 下一行；否则上面的隐形按钮会把光标留在卡内。
    ImGui::SetCursorScreenPos(row_at);
    ImGui::Dummy(ImVec2(total_w, body_h));

    ImGui::EndChild();
}

} // namespace aqua::gui
