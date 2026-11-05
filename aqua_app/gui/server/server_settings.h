#ifndef AQUA_APP_GUI_SERVER_SETTINGS_H
#define AQUA_APP_GUI_SERVER_SETTINGS_H

// 服务端 GUI 的偏好与表单持久化（主题 / 日志视图 / 服务参数表单）。
//
// 落盘位置：SDL_GetPrefPath("aquawius", "aqua-server")/settings.ini
// （Windows: %APPDATA%\aquawius\aqua-server\）。跟 client 的设置完全分开——
// 两个程序的主题选择互不影响。
//
// 表单（地址 / 端口 / 采集设备 / 高级格式）也记住上次的值，但有一条硬规则：
// **命令行显式传过的字段赢**（load 时和默认值比对即可判断）。这样
// `aqua_server_gui --udp-port 50100` 不会被存档悄悄改掉，而"界面里调好的那套"
// 也不用每次启动重敲一遍——ini 不是隐藏配置源，它只是"上次的表单"。

#include <string>

namespace aqua::runtime {
struct ServerRuntimeConfig;
}

namespace aqua::gui {

struct ServerUiState;

[[nodiscard]] std::string server_data_dir();

// cli = 命令行解析出的配置（用于判断哪些字段是显式传的）。
void load_server_settings(ServerUiState& s, const aqua::runtime::ServerRuntimeConfig& cli);
bool save_server_settings(const ServerUiState& s);

} // namespace aqua::gui

#endif // AQUA_APP_GUI_SERVER_SETTINGS_H
