#ifndef AQUA_APP_GUI_UI_MAIN_H
#define AQUA_APP_GUI_UI_MAIN_H

// 桌面 GUI 门面：main.cpp 只需要包含本头。
//
// 文件结构（按依赖方向，从数据到编排）：
//   ui_state.h        纯数据状态（UiState / UiPhase；含接线约定注释）
//   log_buffer.*      运行日志缓冲（线程安全单例）
//   app_style.*       主题与间距度量
//   ui_layout.h       布局惯用法（两个右对齐 helper，各附踩坑注释）
//   ui_panels.h       面板绘制入口契约（ui_main.cpp ↔ 各面板 .cpp）
//   ui_connection.cpp 连接卡片（预设表 / 设备缓存 / 指标条）+ toggle_connection
//   ui_log.cpp        运行日志面板
//   ui_dialogs.cpp    设置 / 关于 / 占位窗
//   ui_menu.cpp       菜单栏
//   ui_demo.*         演示数据（Phase 0 专用，会话接入后整体删除）
//   ui_main.cpp       编排：draw_main_window（菜单栏 + 上下分屏 + 状态栏）
//
// 会话层（ClientRuntime）接入入口：读 UiState「连接设置」段、写「会话状态」段、
// 往 log_buffer() 写日志；UI 侧调用点不用动。

#include "app_style.h"
#include "log_buffer.h"
#include "ui_state.h"

namespace aqua::gui {

// 绘制全屏主窗口（菜单栏 + 上下分屏 + 状态栏 + 附加窗口）。
// 每帧调一次；内部按 ImGui 的即时模式惯例提交全部控件。
void draw_main_window(UiState& state);

// 连接 / 断开（演示模式切换 phase；会话接入后由 session 层驱动）。
// 菜单 文件→连接/断开、连接大按钮、Ctrl+K 快捷键三者共用同一入口。
void toggle_connection(UiState& state);

} // namespace aqua::gui

#endif // AQUA_APP_GUI_UI_MAIN_H
