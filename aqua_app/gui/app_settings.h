#ifndef AQUA_APP_GUI_APP_SETTINGS_H
#define AQUA_APP_GUI_APP_SETTINGS_H

// 界面设置持久化。
//
// 只保存**用户偏好**（主题 / 字体 / 刷新率 / 连接参数 / JB 参数 / 日志视图），
// 不保存运行期状态（会话、指标、日志缓冲、窗口开关）——后者重启后应该从零开始。
//
// 落盘位置：SDL_GetPrefPath("aquawius", "aqua-client") 下的 settings.ini
// （Windows: %APPDATA%\aquawius\aqua-client\，Linux: ~/.local/share/...）。
// 取不到用户目录时回退到 exe 同目录的 aqua-settings.ini。
//
// 格式是最朴素的 key=value 文本（# 开头为注释），不用 JSON：这里只有二十来个
// 标量字段，多一层 JSON 解析只会多一处可能崩的地方。**任何一行坏了都只跳过**，
// 绝不让"配置文件写坏"变成"程序起不来"。

#include <string>

namespace aqua::gui {

struct UiState;

// settings.ini 的完整路径（首次调用时向 SDL 查询并缓存）。
[[nodiscard]] std::string settings_path();

// 读取 → 写进 UiState。缺项 / 非法值一律保留 UiState 里的默认值。
// 文件不存在是正常情况（首次启动），不报错。
void load_settings(UiState& s);

// 把可持久化的字段写盘。先写 .tmp 再原子替换，避免写到一半被强杀时留下半份文件。
// 失败只往运行日志记一行，不抛异常、不阻塞。
bool save_settings(const UiState& s);

} // namespace aqua::gui

#endif // AQUA_APP_GUI_APP_SETTINGS_H
