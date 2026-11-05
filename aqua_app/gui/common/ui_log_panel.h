#ifndef AQUA_APP_GUI_UI_LOG_PANEL_H
#define AQUA_APP_GUI_UI_LOG_PANEL_H

// 运行日志面板（client / server 共用的同一份实现）。
//
// 与具体前端解耦的方式：面板只认 **LogView**（级别过滤 / 自动滚动 / 字号 / 等宽
// 字体 / 导出前缀），不认 UiState——server 端没有 UiState，而日志面板在两边必须
// 长得一模一样（同一行格式、同一套跟随尾部语义）。

#include "log_buffer.h"

#include <imgui.h>

#include <string>

namespace aqua::gui {

struct LogView {
    int min_level = kLogInfo;      // 过滤级别（同时驱动 core 的日志级别，前端负责下发）
    bool autoscroll = true;        // 跟随尾部
    int font_size = 14;            // 日志正文字号（px，10–20）
    ImFont* mono_font = nullptr;   // 等宽字体；空 = 用当前字体
    std::string export_dir;        // 导出目录（带尾分隔符）；空 = 不可导出
    const char* export_prefix = "aqua"; // 导出文件名：<prefix>-YYYYMMDD-HHMMSS.log
    const char* empty_hint = "（暂无日志）";
};

// 在"窗口内容区"里画一个吃满剩余高度的日志面板（自带边框与工具栏）。
// 调用方负责先 Begin 主窗口 / 内容 child；本函数内部 BeginChild/EndChild 成对。
void draw_log_panel(LogView& view);

} // namespace aqua::gui

#endif // AQUA_APP_GUI_UI_LOG_PANEL_H
