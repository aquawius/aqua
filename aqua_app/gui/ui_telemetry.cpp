#include "ui_panels.h"

#include "ui_demo.h"
#include "ui_layout.h"
#include "ui_state.h"

#include <imgui.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

// 遥测面板：连接页的下半屏（连接卡片之下，吃满剩余高度）。
//
// 2026-10-18 第三轮（本轮）版式：整块面板只认**两条竖线 + 一串横线**。
//
//   内容左缘 x0                                                  内容右缘 x1
//   │ 缓冲水位 42.1 %    领先 26.0 ms     目标 26.8 ms            │  ← 指标网格（3 组 / 行 × 2 行）
//   │ 网络抖动  3.2 ms   缺口率 0.01 %    欠载  0.00 %            │     标签列定宽、数值列等宽
//   │ ───────────────────────────────────────────────────────── │  ← 1px 分节线
//   │ 水位 %  ┄┄ 警戒 12 %                                   统计… │  ← 曲线头：左说明+图例 / 右入口
//   │ ┌────────────────────────────────────────────────────────┐ │
//   │ │100├───────────────────────────────────────────────────  │ │  ← 曲线盒：与 x0 / x1 齐平
//   │ │   ├┄┄┄┄┄┄┄┄┄┄ 12 % 欠载警戒线 ┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄  │ │     纵轴刻度画在盒内左侧留白
//   │ │ 50├───────────────────────────────────────────────────  │ │     4×4 方格纸网格
//   │ │  0├───────────────────────────────────────────────────  │ │
//   │ └────────────────────────────────────────────────────────┘ │
//   │ -60 s                                                   现在 │  ← 横轴说明（两端对齐）
//   │ ───────────────────────────────────────────────────────── │
//   │ 音频格式  48 kHz · 立体声 · PCM F32LE · 1536 kbps · …       │  ← 会话固定信息
//
// 为什么这样排：上一版的"曲线盒"左缩进 42px 给纵轴刻度、右缩进 10px、圆角 3px，
// 而上面的指标与下面的音频格式都从内容左缘起、分节线又跨满全宽——四条竖线互不
// 相干，看着就"不整齐"（用户 2026-10-18 反馈）。现在盒子两端与分节线、指标表、
// 页脚**共用同两条竖线**；纵轴刻度让进盒子内部，网格线自己构成方格纸，于是整块
// 面板只剩一个外框 + 内部一张网格，横平竖直。
//
// 刻意**不**在这里显示「已连接 / 会话号 / 运行时长」：那是"现在正在做什么"，
// 归底部状态栏（之前两处都写，用户看着重复）。音频格式反过来——它是本次会话
// 的固定事实，留在可视化里，状态栏不再重复。
//
// 数据全部来自 UiState::session（会话层快照），本文件不碰 core；趋势历史是
// UI 侧派生缓存，属于 TelemetryState。

namespace aqua::gui {
namespace {

// 60 秒历史：500ms 一拍 = 120 点。会话层的发布节拍正是 500ms。
constexpr int kTrendPoints = 120;

// 低水位警戒线：水位掉到这里基本就是欠载前兆。画成虚线，让"数字变小"
// 变成"图形贴近底边"。
constexpr float kLowWaterMark = 0.12f;

struct RecentEndpoint {
    std::string ip;
    std::string port;
    double at = 0.0; // ImGui 时间轴（进程内相对时间）
};

// 遥测面板的派生缓存（丢了只是重画，不属于 UiState）。
struct TelemetryState {
    float water[kTrendPoints] = {};
    int count = 0; // 已采样的点数（≤ kTrendPoints）
    int head = 0;  // 下一个写入位置
    std::uint64_t last_seq = 0;   // 已消费的快照序号（去重用）
    double last_sample = -1.0e9;  // 演示模式的采样节拍
    UiPhase last_phase = UiPhase::Idle;
    std::vector<RecentEndpoint> recent; // 最近连接，最新的在前，最多 3 条
    // 布局实测值：指标网格里"数值列"的起点（窗口内 X）。页脚的音频格式值用它
    // 对齐到同一条竖线上——实测而非手算 CellPadding，字号 / 主题换了也不会错位。
    float value_x = 0.0f;
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

} // namespace

// 秒 → "mm:ss"（≥1h 为 "h:mm:ss"）。遥测面板与状态栏共用，故放在匿名
// namespace 之外（声明在 ui_panels.h）。
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

namespace {

void remember_endpoint(TelemetryState& v, const UiState& s)
{
    const std::string ip = s.rpc_ip;
    const std::string port = s.rpc_port;
    for (auto it = v.recent.begin(); it != v.recent.end(); ++it) {
        if (it->ip == ip && it->port == port) {
            v.recent.erase(it);
            break;
        }
    }
    v.recent.insert(v.recent.begin(), RecentEndpoint { ip, port, ImGui::GetTime() });
    if (v.recent.size() > 3) {
        v.recent.resize(3);
    }
}

std::string format_age(double seconds)
{
    const int total = static_cast<int>(std::max(0.0, seconds));
    if (total < 60) {
        return "刚刚";
    }
    if (total < 3600) {
        return std::to_string(total / 60) + " 分钟前";
    }
    return std::to_string(total / 3600) + " 小时前";
}

// 一条 1px 分节线（跨满内容宽度）+ 上下各一点呼吸量。
// 不用 ImGui::Separator：它的留白来自 ItemSpacing，面板里几条分节的间距要一致就
// 得自己定。整条只提交**一个** item（Dummy），高度 = 1px + 上下 pad，行距按 ImGui
// 的惯例另算——预算里漏算行距会把页脚挤出面板（上一版实测过）。
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

// ---- 水位曲线 ----
// 盒子左 / 右两端与指标表、分节线、页脚严格同宽（都用 GetContentRegionAvail）：
// 纵轴刻度落在盒子**内部**的左侧留白里，网格线自己画成 4×4 方格纸。这样整块
// 面板的竖线只有外面那两条，横向只有分节线与网格线，没有第三条"半吊子"缩进。
//
//   · 纵轴 100 / 50 / 0 右对齐到绘图区左缘（不写 "%"：单位在曲线头"水位 %"里）；
//   · 横轴 4 等分、纵轴 4 等分，25% 一档方格纸；
//   · 12% 欠载警戒线画成红色虚线，图例在曲线头（线上一律不压字，会和曲线打架）；
//   · 曲线与填充取当前水位的健康色（与指标网格的数字同色，看数字和看形状能对上）。
//
// 高度由调用方给定（reserve_below = 盒子下方那张横轴说明 + 分节线 + 页脚要占的
// 像素），这里只负责把"到面板内底缘为止"的剩余空间吃干净——窗口拉高时长大的
// 是曲线，其余各件的位置不动。
//
// 横轴跨度：500ms 一拍、环形缓冲 120 点 = 固定 60 秒滚动窗，**有界**，不会一直
// 累积。不足 120 点时按已采到的点数铺满整幅宽度（否则刚连上的前几秒，曲线会
// 缩在右边缘一小撮，看着像坏了）。
void draw_trend_box(const TelemetryState& v, float height, float k)
{
    const float width = ImGui::GetContentRegionAvail().x;
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(width, height));
    // 太窄 / 太矮就不画：刻度挤在一起比没有刻度更难读。
    if (width < 120.0f * k || height < 40.0f * k) {
        return;
    }

    const ImGuiStyle& st = ImGui::GetStyle();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    const ImVec2 b0 = p0;
    const ImVec2 b1(p0.x + width, p0.y + height);

    // 纵轴留白：宽度按最宽刻度实测，字号变了也不会切字。
    const float tick_w = ImGui::CalcTextSize("100").x;
    const ImVec2 g0(b0.x + tick_w + st.ItemInnerSpacing.x * 2.0f, b0.y + 1.0f);
    const ImVec2 g1(b1.x - k, b1.y - 1.0f);
    const float ph = g1.y - g0.y;
    const float pw = g1.x - g0.x;

    // 盒底 + 1px 方角外框（不圆角：窗口里 tab / 输入框 / 表格全是方角）。
    dl->AddRectFilled(b0, b1, ImGui::GetColorU32(ImGuiCol_FrameBg, 0.30f));
    dl->AddRect(b0, b1, ImGui::GetColorU32(ImGuiCol_Border));

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

    // 纵轴刻度：0 贴底、100 贴顶、50 居中（两端对齐而不是都居中，字才不会探出框外）。
    const float th = ImGui::GetTextLineHeight();
    const ImU32 dim = ImGui::GetColorU32(ImGuiCol_TextDisabled, 0.85f);
    struct Tick {
        float frac;
        const char* text;
    };
    const Tick ticks[3] = { { 1.0f, "100" }, { 0.5f, "50" }, { 0.0f, "0" } };
    for (const Tick& t : ticks) {
        const float tw = ImGui::CalcTextSize(t.text).x;
        float y = g1.y - ph * t.frac - th * 0.5f;
        if (t.frac >= 1.0f) {
            y = g0.y;
        } else if (t.frac <= 0.0f) {
            y = g1.y - th;
        }
        dl->AddText(ImVec2(g0.x - st.ItemInnerSpacing.x - tw, y), dim, t.text);
    }

    // 欠载警戒线（虚线）。**不**在线上压字：水位正常时那条线正好躺在曲线下方
    // 一点，任何行内标注都会和数据打架；含义放到曲线头右侧做图例（见
    // draw_trend_block），线本身只留形状。
    const float y_low = g1.y - ph * kLowWaterMark;
    dashed_hline(dl, ImVec2(g0.x + 1.0f, y_low), ImVec2(g1.x - 1.0f, y_low),
        ImGui::GetColorU32(ImVec4(0.94f, 0.42f, 0.38f, 0.55f)), 3.0f * k, 3.0f * k);

    if (v.count < 2) {
        // 只有 0~1 个点：画个居中的提示，别让用户以为图坏了。
        const char* hint = "等待第一帧诊断数据…";
        const ImVec2 ts = ImGui::CalcTextSize(hint);
        dl->AddText(ImVec2(g0.x + (pw - ts.x) * 0.5f, g0.y + (ph - ts.y) * 0.5f),
            ImGui::GetColorU32(ImGuiCol_TextDisabled), hint);
        return;
    }

    // 数据（裁到绘图区内：折线 / 圆点绝不越过盒子边框）。
    const float step = pw / static_cast<float>(v.count - 1);
    ImVec2 line[kTrendPoints];
    for (int i = 0; i < v.count; ++i) {
        const float x = g1.x - step * static_cast<float>(v.count - 1 - i);
        line[i] = ImVec2(x, g1.y - ph * water_at(v, i));
    }

    const ImVec4 c = water_color(static_cast<double>(water_at(v, v.count - 1)));
    const ImU32 col = ImGui::GetColorU32(c);
    const ImU32 fill = ImGui::GetColorU32(ImVec4(c.x, c.y, c.z, 0.16f));

    dl->PushClipRect(ImVec2(g0.x, g0.y), ImVec2(g1.x, g1.y), true);
    // 曲线下方填充（AddConcavePolyFilled：折线下方不是凸多边形）
    ImVec2 area[kTrendPoints + 2];
    area[0] = ImVec2(line[0].x, g1.y);
    for (int i = 0; i < v.count; ++i) {
        area[i + 1] = line[i];
    }
    area[v.count + 1] = ImVec2(line[v.count - 1].x, g1.y);
    dl->AddConcavePolyFilled(area, v.count + 2, fill);
    dl->AddPolyline(line, v.count, col, ImDrawFlags_None, 1.5f);
    // 采样点少（< 12 个 = 刚连上的头 6 秒）时补一圈小圆点：这时的折线被拉满
    // 整幅宽度，光看线分不清"只有两个采样"还是"一路平稳"，点出来就诚实了。
    if (v.count <= 12) {
        for (int i = 0; i < v.count; ++i) {
            dl->AddCircleFilled(line[i], 2.5f * k, col, 8);
        }
    }
    // 末端圆点 = "现在这一拍"
    dl->AddCircleFilled(line[v.count - 1], 3.0f * k, col);
    dl->PopClipRect();
}

// 曲线整块 = 头（左"水位 %" / 右「统计…」入口）+ 盒子 + 横轴说明。
// 盒子高度 = 到面板内底缘为止的剩余高度 - reserve_below（下面几件要占的像素）：
// 用绝对底缘算，不用"逐件累加"，所以上面各件的行距怎么变都不会把页脚挤出面板。
void draw_trend_block(const TelemetryState& v, UiState& s, float reserve_below)
{
    const float k = (s.dpi_scale > 0.0f) ? s.dpi_scale : 1.0f;

    // 横轴跨度：500ms 一拍，满窗 120 点 = 59.5 s（说明里写 60 秒）。
    const float span_s = static_cast<float>(std::max(v.count - 1, 1)) * 0.5f;
    const float shown_span = (v.count >= 2) ? span_s : 60.0f;

    // ---- 头 ----
    // 注意：**不要** AlignTextToFramePadding。这一行全是文本（没有 framed 控件），
    // 对齐没意义；可它会把行高从 FontSize 抬到 FontSize + 2·FramePadding.y（10px），
    // 页脚那行因此比预算高出一截，面板就会凭空长出一条滚动条（实测）。
    ImGui::TextDisabled("%s", "水位 %");
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("JitterBuffer 缓冲水位（占用槽位 / 容量），最近 %.0f 秒的走势。", shown_span);
    }
    // 警戒线图例：先画一小段和盒子里一致的虚线当色标，再写含义。行内标注会和
    // 曲线打架，所以含义放这儿；色标让"这行字说的是盒子里那条虚线"不用猜。
    const ImGuiStyle& st = ImGui::GetStyle();
    const float text_h = ImGui::GetTextLineHeight();
    const float swatch_w = 16.0f * k;
    ImGui::SameLine(0.0f, st.ItemInnerSpacing.x * 2.0f);
    ImVec2 sw_at = ImGui::GetCursorScreenPos();
    dashed_hline(ImGui::GetWindowDrawList(),
        ImVec2(sw_at.x, sw_at.y + text_h * 0.5f),
        ImVec2(sw_at.x + swatch_w, sw_at.y + text_h * 0.5f),
        ImGui::GetColorU32(ImVec4(0.94f, 0.42f, 0.38f, 0.55f)), 3.0f * k, 3.0f * k);
    ImGui::Dummy(ImVec2(swatch_w, 0.0f));
    ImGui::SameLine(0.0f, st.ItemInnerSpacing.x);
    ImGui::TextColored(ImVec4(0.94f, 0.42f, 0.38f, 0.85f), "%s", "警戒 12 %");
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("缓冲水位掉到 12%% 以下基本就是欠载前兆（虚线位置）。");
    }
    // 右侧入口：统计详表在设置框里（文字链，比按钮轻；与左说明同一行、右缘贴齐）。
    const float link_w = ImGui::CalcTextSize("统计…").x;
    same_line_right(link_w);
    if (ImGui::TextLink("统计…")) {
        s.settings_tab = kSettingsStats;
        s.show_settings = true;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("打开设置 → 统计（指标口径、会话固定信息与 core 诊断明细）");
    }

    // ---- 盒子 ----
    const ImVec2 box_at = ImGui::GetCursorScreenPos();
    const float panel_bottom = ImGui::GetWindowPos().y + ImGui::GetWindowContentRegionMax().y;
    const float box_h = std::max(panel_bottom - box_at.y - reserve_below, 40.0f * k);
    draw_trend_box(v, box_h, k);

    // ---- 横轴说明：左端 "-60 s"、右端 "现在"，两端与盒子左右缘对齐 ----
    // 不足 60 秒时左端写真实跨度（0.5 s 一拍，所以带一位小数）。
    if (shown_span < 9.95f) {
        ImGui::TextDisabled("-%.1f s", shown_span);
    } else {
        ImGui::TextDisabled("-%.0f s", shown_span);
    }
    const char* now_text = "现在";
    same_line_right(ImGui::CalcTextSize(now_text).x);
    ImGui::TextDisabled("%s", now_text);
}

// ---- 音频格式页脚 ----
// 内容是本次会话的固定事实（采样率 · 声道 · 编码 · 码率 · 帧长），连上就不变，
// 所以放在最下面当"页脚"，不跟每秒都在跳的水位争位置。
// 值的那一列强行对齐到指标网格的"数值列"（TelemetryState::value_x）：整块面板
// 因此只有 x0 / 数值列 / x1 三条竖线。
// 演示模式没有真实格式，给一套自洽的拟真值。
void draw_format_row(const UiState& s, float value_x)
{
    const std::string& fmt = s.demo
        ? std::string("48 kHz · 立体声 · PCM F32LE · 1536 kbps · 480 帧 ≈ 10.0 ms")
        : s.session.audio_format;

    // 整行只有文本：不调 AlignTextToFramePadding（它会把行高抬 2·FramePadding.y，
    // 预算里按 text_h 算就短了 10px，面板会长出滚动条）。
    ImGui::TextDisabled("音频格式");
    const ImGuiStyle& st = ImGui::GetStyle();
    const float label_right = ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x
        + st.ItemInnerSpacing.x;
    if (fmt.empty()) {
        ImGui::SameLine(label_right);
        ImGui::TextDisabled("—");
        return;
    }

    // 数值列起点：优先用指标网格量出来的 x（整块面板只有三条竖线）；窗口被压得
    // 比那条线还窄时退回"标签右缘 + 常规间距"，不至于压住标签。
    const float x = std::max(value_x, label_right);
    ImGui::SameLine(x);
    ImGui::PushFont(s.font_mono, mono_size(s));
    ImGui::TextUnformatted(fmt.c_str()); // 正文是数字串：用等宽，数字才对得齐
    ImGui::PopFont();
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("音频格式：采样率 · 声道 · 编码 · 码率 · 帧长\n"
                          "由服务器在连接应答里通告，本次会话内不变。");
    }
}

// ---- 未连接时的下半屏：曲线占位框 + 最近连接（点一行回填地址）----
// 占位框不是装饰：它标出"曲线会长在这里"，并把剩余高度吃掉，卡片因此不会
// 出现一大片没有意义的空白。
// 注意：**不在这里显示"正在连接 / 正在重连"**——那些属于"现在正在做什么"，
// 放底部状态栏（用户 2026-10-18）。面板只回答"连接后会长出什么"。
void draw_idle_body(UiState& s, TelemetryState& v)
{
    const ImGuiStyle& st = ImGui::GetStyle();
    const float s_y = st.ItemSpacing.y;
    const float text_h = ImGui::GetTextLineHeight();
    const float row_h = text_h + st.FramePadding.y * 2.0f; // Selectable 行高

    // 占位框的高度 = 到面板内底缘为止的剩余高度 - 下方几件要占的像素。和已连接态
    // 一样按"绝对底缘 - 逐 item 预算"算，别用"减去一个估的行数"：估错几像素就会
    // 凭空长出一条滚动条（未连接态曾因此被顶出一条）。
    const int rows = static_cast<int>(v.recent.size());
    const float reserve = s_y                              // 占位框自己的行距
        + 4.0f + s_y                                       // 框下的空隙
        + row_h + s_y                                      // 「最近连接」标题行（含按钮）
        + (rows > 0 ? static_cast<float>(rows) * (row_h + s_y) : text_h + s_y);

    const float panel_bottom = ImGui::GetWindowPos().y + ImGui::GetWindowContentRegionMax().y;
    const float box_h = panel_bottom - ImGui::GetCursorScreenPos().y - reserve;
    if (box_h > 40.0f) {
        const float width = ImGui::GetContentRegionAvail().x;
        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        ImGui::Dummy(ImVec2(width, box_h));
        ImDrawList* dl = ImGui::GetWindowDrawList();
        // 方角 + 1px 边：与已连接态的曲线盒同一套几何，切换时位置不跳。
        dl->AddRectFilled(p0, ImVec2(p0.x + width, p0.y + box_h),
            ImGui::GetColorU32(ImGuiCol_FrameBg, 0.28f));
        dl->AddRect(p0, ImVec2(p0.x + width, p0.y + box_h), ImGui::GetColorU32(ImGuiCol_Border));
        const char* hint = (s.phase == UiPhase::Connecting) ? "正在建立连接…"
            : (s.phase == UiPhase::Disconnecting)          ? "正在断开…"
            : (s.phase == UiPhase::Failed)                 ? "连接失败，详见运行日志"
                                                           : "连接后这里显示 60 秒缓冲水位曲线";
        const ImVec2 ts = ImGui::CalcTextSize(hint);
        dl->AddText(ImVec2(p0.x + (width - ts.x) * 0.5f, p0.y + (box_h - ts.y) * 0.5f),
            ImGui::GetColorU32(ImGuiCol_TextDisabled), hint);
        ImGui::Dummy(ImVec2(0.0f, 4.0f));
    }

    // 「最近连接」标题行右侧放「统计…」，与已连接态把入口放曲线头右侧保持一致。
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("最近连接");
    same_line_right(ImGui::CalcTextSize("统计…").x);
    if (ImGui::TextLink("统计…")) {
        s.settings_tab = kSettingsStats;
        s.show_settings = true;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("打开设置 → 统计（指标口径与实时值）");
    }

    if (v.recent.empty()) {
        ImGui::TextDisabled("暂无");
        return;
    }

    const float width = ImGui::GetContentRegionAvail().x;
    for (std::size_t i = 0; i < v.recent.size(); ++i) {
        const RecentEndpoint& ep = v.recent[i];
        const std::string addr = ep.ip + ":" + ep.port;
        const std::string age = format_age(ImGui::GetTime() - ep.at);

        const ImVec2 row = ImGui::GetCursorScreenPos();
        ImGui::PushID(static_cast<int>(i));
        if (ImGui::Selectable("##recent", false, ImGuiSelectableFlags_None, ImVec2(0.0f, row_h))) {
            std::snprintf(s.rpc_ip, sizeof(s.rpc_ip), "%s", ep.ip.c_str());
            std::snprintf(s.rpc_port, sizeof(s.rpc_port), "%s", ep.port.c_str());
            s.action_error.clear();
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("点击填回连接参数");
        }
        ImGui::PopID();

        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float text_y = row.y + (row_h - ImGui::GetTextLineHeight()) * 0.5f;
        dl->AddText(ImVec2(row.x + 2.0f, text_y), ImGui::GetColorU32(ImGuiCol_Text), addr.c_str());
        const float age_w = ImGui::CalcTextSize(age.c_str()).x;
        dl->AddText(ImVec2(row.x + width - age_w - 2.0f, text_y),
            ImGui::GetColorU32(ImGuiCol_TextDisabled), age.c_str());
    }
}

} // namespace

void draw_telemetry_panel(UiState& s)
{
    TelemetryState& view = telemetry_state();
    const bool connected = (s.phase == UiPhase::Connected);

    // 阶段迁移：进入 Connected 视为"新会话"——清趋势、记一条最近连接。
    // （重连成功后也算新会话：曲线从头开始比接着断线前的画更诚实。）
    if (s.phase != view.last_phase) {
        if (connected) {
            view.count = 0;
            view.head = 0;
            view.last_seq = 0;
            view.last_sample = -1.0e9;
            remember_endpoint(view, s);
            // 演示模式：把"过去 60 秒"补出来。真实会话没有这段历史（不能编），
            // 但演示模式整屏都是拟真数据，空着一条直线反而像功能坏了。
            if (s.demo) {
                const double now = ImGui::GetTime();
                for (int i = kTrendPoints - 1; i >= 0; --i) {
                    double d[6];
                    fill_demo_tiles_at(now - static_cast<double>(i) * 0.5, d);
                    push_water(view, d[0]);
                }
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
        } else if (s.session.snapshot_seq != view.last_seq) {
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

    // 不开 NoScrollbar：窗口被拖得很矮时曲线盒有 40px 下限，内容会超出面板高度，
    // 宁可让 ImGui 出一条滚动条（页脚还能滚出来），也别把音频格式行永久裁掉。
    if (!ImGui::BeginChild("##telemetry", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders)) {
        // 没有剩余高度（窗口很矮）：跳过内容，但 EndChild 必须照调——BeginChild
        // 已经把子窗口压栈了。
        ImGui::EndChild();
        return;
    }

    if (!connected) {
        // 未连接：不摆一排 "-" 占位（那不是信息，是噪音），只给占位框 + 最近连接。
        draw_idle_body(s, view);
        ImGui::EndChild();
        return;
    }

    // ---- 已连接：指标网格 → 分节线 → 曲线（吃满剩余）→ 分节线 → 音频格式 ----
    const float k = (s.dpi_scale > 0.0f) ? s.dpi_scale : 1.0f;
    const ImGuiStyle& st = ImGui::GetStyle();
    const float pad = 3.0f * k;
    const float text_h = ImGui::GetTextLineHeight();
    const float spacing_y = st.ItemSpacing.y;

    draw_metrics_grid(s, &view.value_x);
    section_rule(pad);

    // 曲线盒下方要留的像素。行距是 ImGui 每提交一个 item 就推进一次的，这里按
    // item 逐项算清楚——少算一次就会把页脚挤出面板 / 凭空长出一条滚动条（实测）。
    const float reserve_below = spacing_y                // 盒子自己的行距
        + (text_h + spacing_y)                           // 横轴说明
        + (rule_item_height(pad) + spacing_y)            // 分节线
        + text_h + 4.0f;                                 // 页脚 + 余量

    draw_trend_block(view, s, reserve_below);

    section_rule(pad);
    draw_format_row(s, view.value_x);

    ImGui::EndChild();
}

} // namespace aqua::gui
