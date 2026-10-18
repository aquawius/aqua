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
//   指标网格（3 组 / 行 × 2 行，标签与数值同一行）
//   ─ 分节线 ─
//   曲线头（左「水位 %」+ 警戒线图例，右「统计…」入口）
//   水位曲线（4×4 方格纸 + 纵轴刻度画在盒内 + 12% 欠载警戒虚线）
//   横轴说明（左 -60 s / 右 现在）
//   ─ 分节线 ─
//   音频格式（会话固定事实；值对齐到指标网格的数值列）
// 「已连接 / 会话号 / 运行时长」不在这里——那些属于"现在正在做什么"，在底部
// 状态栏（避免两处重复）。未连接时退化为占位框 + 最近连接。
void draw_telemetry_panel(UiState& s);

// 运行日志面板（独立 tab 页）：工具栏（级别 / 自动滚动 / 清空）+ 基础日志列表
// （一行一条、只画视口内的行）。高度吃满剩余空间。
void draw_log_panel(UiState& s);

// 设置窗口（模态，多 tab：外观 / 统计 / 高级）。
void draw_settings_window(UiState& s);

// 关于窗口（模态）。
void draw_about_window(UiState& s);

// 指标：连接页遥测面板里的网格（3 组 / 行 × 2 行，标签与数值同行）与设置→统计
// 的详表（带口径说明）。value_x 回填"数值列"的窗口内 X（实测值，供页脚对齐到
// 同一条竖线）；传 nullptr = 不需要。
void draw_metrics_grid(const UiState& s, float* value_x = nullptr);
void draw_stats_tab(const UiState& s);

// 等宽字号（指标数值 / 遥测面板数字）：UI 基准字号 × 0.875。
// PushFont 收的是"未缩放"基准值，FontScaleDpi 会再统一乘，所以这里不掺 DPI。
float mono_size(const UiState& s);

// 水位健康色（<12% 红 / <25% 黄 / 其余正常前景色）：指标条的数字与遥测曲线
// 共用，阈值与曲线上的"欠载警戒线"一致——看数字和看形状能直接对上。
ImVec4 water_color(double level);

// 秒 → "mm:ss"（≥1h 为 "h:mm:ss"）。状态栏与遥测面板共用。
std::string format_duration(double seconds);

} // namespace aqua::gui

#endif // AQUA_APP_GUI_UI_PANELS_H
