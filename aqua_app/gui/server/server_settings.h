#ifndef AQUA_APP_GUI_SERVER_SETTINGS_H
#define AQUA_APP_GUI_SERVER_SETTINGS_H

// 服务端 GUI 的偏好与表单持久化（主题 / 日志视图 / 服务参数表单）。
//
// 落盘位置：SDL_GetPrefPath("aquawius", "aqua-server")/settings.ini
// （Windows: %APPDATA%\aquawius\aqua-server\）。跟 client 的设置完全分开——
// 两个程序的主题选择互不影响。
//
// 表单（地址 / 端口 / 采集设备 / 高级格式）也记住上次的值，但有一条硬规则：
// **命令行显式传过的字段赢**。是否显式由 detect_server_cli_overrides 扫 argv
// 得出（不能拿"解析结果 != 默认值"反推：显式传默认值时两者一样）。这样
// `aqua_server_gui --udp-port 50000` 不会被存档悄悄改掉，而"界面里调好的那套"
// 也不用每次启动重敲一遍——ini 不是隐藏配置源，它只是"上次的表单"。

#include <string>

namespace aqua::runtime {
struct ServerRuntimeConfig;
}

namespace aqua::gui {

struct ServerUiState;

// 命令行里**显式出现过**的服务参数名。不能拿"解析结果 != 默认值"来反推：
// `--udp-port 50000`（恰好等于默认值）与"没传"产生同一个结果，但前者必须赢过
// 存档。这里只探测选项名是否出现，不解析值——值仍由 cli_parser_server 单一负责。
struct ServerCliOverrides {
    bool rpc_ip = false;
    bool rpc_port = false;
    bool udp_ipv4 = false;
    bool udp_ipv6 = false;
    bool udp_port = false;
    bool capture_source = false;
    bool capture_device = false;
    bool audio_format = false;
    bool packet_frames = false;
};

[[nodiscard]] ServerCliOverrides detect_server_cli_overrides(int argc, char** argv);

[[nodiscard]] std::string server_data_dir();

// cli = 命令行解析出的配置；overrides = 哪些字段是命令行显式给的（优先于存档）。
void load_server_settings(ServerUiState& s, const aqua::runtime::ServerRuntimeConfig& cli,
    const ServerCliOverrides& overrides);
bool save_server_settings(const ServerUiState& s);

} // namespace aqua::gui

#endif // AQUA_APP_GUI_SERVER_SETTINGS_H
