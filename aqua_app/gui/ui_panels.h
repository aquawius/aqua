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

namespace aqua::gui {

struct UiState;

// 菜单栏（viewport 级主菜单：文件 / 视图 / 设置 / 帮助）。
void draw_menu_bar(UiState& s);

// 连接卡片（上半区）：地址表 / 播放设备 / 固定使用此设备 / 网络预设 /
// 自定义配置 + 高级参数 / 连接大按钮 / 指标条。高度自适应内容。
void draw_connection_card(UiState& s);

// 运行日志面板（下半区）：工具栏（级别 / 字体大小 / 自动滚动 / 清空）+
// 日志列表。高度吃满剩余空间。
void draw_log_panel(UiState& s);

// 设置窗口（模态）：主题 / 字体 / 运行日志 / 连接 / 恢复默认。
void draw_settings_window(UiState& s);

// 关于窗口（模态）。
void draw_about_window(UiState& s);

// 两个 Phase 0 占位窗（诊断 / 高级设置）。文案随面板落地逐个替换。
void draw_placeholder_windows(UiState& s);

} // namespace aqua::gui

#endif // AQUA_APP_GUI_UI_PANELS_H
