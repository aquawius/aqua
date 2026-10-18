#ifndef AQUA_APP_GUI_UI_PANELS_H
#define AQUA_APP_GUI_UI_PANELS_H

// 各面板的绘制入口声明——ui_main.cpp（编排层）与各面板 .cpp 之间的契约。
// main.cpp 不认本头，它只认 ui_main.h。
//
// 调用约定：所有入口都假定调用方已经 Begin 了主窗口（##aqua_main）与内容
// child（##content），即在"窗口内容区"里提交控件；模态弹窗类入口自行
// OpenPopup + BeginPopupModal，对调用时机不敏感（draw_main_window 尾部统一调）。
//
// 面板自有状态：各面板 .cpp 顶部有具名的 *PanelState 结构（设备列表、日志
// 快照缓存、布局实测值等派生数据），不放 UiState——那是纯配置/会话状态。
// 加新的面板级缓存时，请照样具名放结构里，别再散落函数内 static。

#include <imgui.h>

#include <string>

namespace aqua::gui {

struct UiState;

// 菜单栏（文件 / 设置 / 帮助）。
void draw_menu_bar(UiState& s);

// 连接卡片（连接页上半区）：地址表 / 播放设备 / 固定使用此设备 / 网络预设 /
// 自定义配置 + 高级参数 / 连接大按钮。高度自适应内容。
void draw_connection_card(UiState& s);

// 遥测面板（连接页下半区）：吃满剩余高度，整块只认两条竖线（内容左缘 / 右缘）。
// **结构与会话阶段无关**——没连上时用 "-" / "—" 占位，连上后原地换成数字：
//   音频格式行（会话固定事实；未连接显示 "—"）            …………………… 统计…
//   ─ 分节线 ─
//   两张等高并排卡：
//     左 = 「缓冲」卡：6 个缓冲 / 播放指标（3 行 × 2 组）+ 水位曲线（4×4 方格纸、
//          卡内纵轴刻度、12% 欠载警戒虚线、卡内横轴说明）
//     右 = 右侧卡：8 行「标签 … 数值」（抖动 / 缺口率 / 到达速率 / 接收量 / 发送量 /
//          乱序·迟到 / 心跳存活 / 播放状态）——不限于链路，凡是"不看曲线也要随时
//          可见"的量都放这里
// 「已连接 / 会话号 / 运行时长」不在这里——那些属于"现在正在做什么"，在底部
// 状态栏（避免两处重复）。
void draw_telemetry_panel(UiState& s);

// 运行日志面板（独立 tab 页）：工具栏（级别 / 自动滚动 / 导出… / 清空）+ 基础日志
// 列表（一行一条、只画视口内的行）。高度吃满剩余空间。
void draw_log_panel(UiState& s);

// 设置窗口（模态，多 tab：外观 / 连接 / 统计 / 高级）。
void draw_settings_window(UiState& s);

// 关于窗口（模态）。
void draw_about_window(UiState& s);

// ---- 指标：面板卡片与设置→统计共用的唯一口径表 ----
// 顺序 = 面板里的分组顺序（先「缓冲」卡 6 项，再右侧卡 8 项）。加指标时改
// ui_metrics.cpp 的 kMetricSpecs 一处即可，面板、统计页、tooltip 会一起跟上。
// 右侧卡不限于"链路"：只要是不看曲线也需要随时可见的量（心跳存活、播放状态、
// 收发量）都放这儿——它是面板的第二个信息位，不是"链路专属"。
enum class MetricId {
    // 左侧「缓冲」卡
    WaterPct,  // 缓冲水位 %
    Slots,     // 占用 / 容量（槽）
    Lead,      // 缓冲领先
    Target,    // 自适应目标
    Underrun,  // 欠载占比
    Glitches,  // 重锚 / 掩盖
    // 右侧卡
    Jitter,      // 网络抖动
    Gap,         // 缺口率
    RxRate,      // 到达速率（包/秒）
    RxBytes,     // 累计接收字节
    TxBytes,     // 累计发送字节
    ReorderLate, // 乱序 / 迟到
    Heartbeat,   // 心跳存活状态（**不**显示"距上次 ACK"那个相位量）
    Playback,    // 本机播放是否在跑
};

inline constexpr int kMetricIdCount = 14;
inline constexpr int kBufferedMetricCount = 6; // 前 6 个属于「缓冲」卡
inline constexpr int kLinkMetricCount = kMetricIdCount - kBufferedMetricCount;

// 指标的显示名 / 口径说明（tooltip 与统计页「说明」列共用）。
const char* metric_label(MetricId id);
const char* metric_desc(MetricId id);
// 当前显示文本（无数据 = "-"；演示模式给拟真值）。
std::string metric_text(const UiState& s, MetricId id);
// 健康色（与曲线、状态栏同一套阈值）。
ImVec4 metric_color(const UiState& s, MetricId id);

// 指标网格（标签与数值同行；pairs_per_row 组 / 行）：面板「缓冲」卡用它。
// width = 网格可用宽度（卡片内宽，不是窗口宽——卡片比面板窄）。
// value_x 回填第一组数值列的窗口内 X，传 nullptr = 不需要。
void draw_metric_grid(const UiState& s, const MetricId* ids, int count, int pairs_per_row,
    float width, float* value_x = nullptr);

// 设置 → 统计页（实时指标全表 + 音频格式 + 会话 / 网络 / 传输 / 心跳 / 抖动缓冲）。
void draw_stats_tab(const UiState& s);

// 等宽字号（指标数值 / 遥测面板数字）：UI 基准字号 × 0.875。
// PushFont 收的是"未缩放"基准值，FontScaleDpi 会再统一乘，所以这里不掺 DPI。
float mono_size(const UiState& s);

// 等宽数值和同行普通标签**共用基线**要往下挪的像素。
// ImGui 的文本是顶对齐的：两种字体 ascent 不同（实测 16px 雅黑标签旁边的 14px
// Consolas 数值会浮高 4px），直接顶对齐看着就是"数字比标签高一点"。这里按两者
// 的 baked ascent 之差给补偿值，字号改了就跟着变。
float mono_baseline_dy(const UiState& s);

// 水位健康色（<12% 红 / <25% 黄 / 其余正常前景色）：指标数字与遥测曲线共用，
// 阈值与曲线上的"欠载警戒线"一致——看数字和看形状能直接对上。
ImVec4 water_color(double level);

// 秒 → "mm:ss"（≥1h 为 "h:mm:ss"）。状态栏与统计页共用。
std::string format_duration(double seconds);

// 字节数 → "12.3 MB"（统计页 / 链路卡共用，裸数字读不出量级）。
std::string format_bytes(std::uint64_t bytes);

} // namespace aqua::gui

#endif // AQUA_APP_GUI_UI_PANELS_H
