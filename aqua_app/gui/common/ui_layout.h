#ifndef AQUA_APP_GUI_UI_LAYOUT_H
#define AQUA_APP_GUI_UI_LAYOUT_H

// 布局惯用法：右对齐助手。本文件只放"跨面板复用 + 有坑"的小工具，
// 每个都附根因注释——这两个 helper 各踩过一次坑，别再裸写 SameLine /
// SetCursorPosX。

#include <imgui.h>

namespace aqua::gui {

// 把后续控件排到"当前行右侧、距内容右缘 width 像素"处（与当前行控件并排的
// 右对齐用：状态栏右侧文本、日志工具栏的「清空」）。
// 关键坑：SetCursorPosX 只改 X——ItemSize 末尾会清掉 IsSameLine
// （imgui.cpp:11622），下一个 item 仍另起一行：状态栏右侧文本曾因此掉到
// 窗口底缘之外（用户报"最下面显示超出范围，而且不对齐"）。必须用
// SameLine(offset) 回到同一行：offset 相对"窗口左缘 - 滚动"
// （imgui.cpp:11646），而 GetWindowContentRegionMax() 返回的正是相对
// window->Pos 的坐标（其源码：ContentRegionRect.Max - window->Pos），两者
// 基准一致；Y 由 SameLine 内部置回 CursorPosPrevLine.y（imgui.cpp:11647）。
// spacing 传 0 不再附加间距。
inline void same_line_right(float width)
{
    ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - width, 0.0f);
}

// 把后续控件排到"下一行右侧、距内容右缘 width 像素"处（跟在 full-width 控件
// 之后的整行右对齐用：连接卡片里的「固定使用此设备」「自定义配置」排）。
// 与 same_line_right 的分工：
//   same_line_right   —— 和当前行控件并排（SameLine 把 Y 拽回上一行）；
//   set_cursor_right_x —— 就在下一行，只改 X。
// 关键坑（2026-10-16 用户截图实测：勾选框压住播放设备下拉框、压住网络预设
// 滑条）：跟在 full-width 控件（下拉框 / 滑条）后面用 same_line_right，会把
// 本该在下一行的控件拽回上一行、与那个控件重叠。此时上一个 item 提交后光标
// 已经停在下一行（ItemSize 的推进），用 SetCursorPosX 只调 X 即可——它不置
// IsSameLine，而 IsSameLine 刚被 ItemSize 清掉（imgui.cpp:11622），下一个
// item 自然落在当前行。
inline void set_cursor_right_x(float width)
{
    ImGui::SetCursorPosX(ImGui::GetWindowContentRegionMax().x - width);
}

} // namespace aqua::gui

#endif // AQUA_APP_GUI_UI_LAYOUT_H
