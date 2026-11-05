#ifndef AQUA_APP_GUI_SERVER_UI_SERVER_H
#define AQUA_APP_GUI_SERVER_UI_SERVER_H

// 服务端 GUI 的编排层（server/main.cpp 只需要包含本头）。
//
// 页面结构与 client 同构，只是内容按"服务端最需要一眼看到的几件事"裁剪：
//   菜单栏（文件 / 设置 / 帮助）
//   └ 主窗口
//     ├ tab：服务 | 运行日志
//     │   ├ 服务：参数卡片（控制地址 / 端口 / 数据面逐族绑定 / 采集设备 / 大按钮）
//     │   │        + 运行信息卡（音频格式一行 + 四行采集读数，一列对齐）
//     │   │        + 会话卡（吃满剩余高度；表内滚动）
//     │   └ 日志页：与 client 完全相同的日志面板
//     └ 状态栏（● 阶段 · 控制端点 · 数据面实际绑定 ……………… 运行时长）
//
// 统计与高级设置都在「设置」模态里（外观 / 统计 / 高级），主页面不常驻明细。
// 音频格式与每包帧数 F 只在「高级设置」里编辑（平时不改）；运行信息卡上的那一行
// 只负责显示这次运行实际用的格式。

#include "server_ui_state.h"

namespace aqua::gui {

void draw_server_window(ServerUiState& s);

// 启动 / 停止切换（菜单与配置卡上的大按钮共用同一入口）。
// 启动前会把表单校验并组装成 ServerRuntimeConfig；失败时写 s.form_error 并返回。
void toggle_server(ServerUiState& s);

// 用命令行解析出的配置初始化表单（main 在首帧前调用一次）。
void load_form_from_config(ServerUiState& s, const aqua::runtime::ServerRuntimeConfig& config);

// 采集设备枚举（停止时每 3s 一次；方向随 loopback/input 变化）。draw 内部会调。
void poll_capture_devices(ServerUiState& s);

} // namespace aqua::gui

#endif // AQUA_APP_GUI_SERVER_UI_SERVER_H
