#ifndef AQUA_APP_GUI_SERVER_UI_STATE_H
#define AQUA_APP_GUI_SERVER_UI_STATE_H

// 服务端 GUI 的全部可变状态（纯数据：无 ImGui 调用、无业务逻辑）。
//
// 与 client 的 UiState 刻意分开：服务端没有"连接参数 / 播放设备 / JB 预设"，
// 共享一个结构只会让两边都背上一堆用不到的字段。共享的是**表现层**（主题、
// 字体、日志面板、格式化），不是状态。

#include "app_style.h"
#include "server_session.h"
#include "ui_log_panel.h"
#include "ui_prefs.h"

#include <aqua/runtime/server_runtime.h>

#include <imgui.h>

#include <string>
#include <vector>

namespace aqua::gui {

// 页面 tab 的下标（page_request 用；main.cpp 的环境变量解析也按它映射）。
enum ServerPage : int {
    kPageService = 0, // 服务：参数卡片 + 两行摘要 + 连接会话
    kPageLog = 1,     // 运行日志
};

// 设置窗口的 tab（settings_tab 请求位，与 client 的三个 tab 同名同序）。
enum ServerSettingsTab : int {
    kSettingsAppearance = 0, // 外观：主题 / 密度 / 圆角 / 字号 / 刷新率 / 日志
    kSettingsStats = 1,      // 统计：core 全量诊断
    kSettingsAdvanced = 2,   // 高级：音频格式 / 每包帧数 F
};

struct ServerUiState {
    // ---- 环境 ----
    ImFont* font_mono = nullptr; // 等宽字体（日志 / 数值）

    // ---- 外观（视图菜单实时切换）----
    Theme theme = Theme::Dark;
    Density density = Density::Normal;
    float rounding = 5.0f; // 圆角像素（0–10）
    int font_size = 16;    // UI 基准字号（px，12–24）
    float dpi_scale = 1.0f;
    int frame_rate_mode = 0;  // 见 ui_prefs.h
    float render_fps = 0.0f;  // 实测渲染帧率（设置 → 外观显示"当前 N fps"）

    // ---- 日志面板视图 ----
    LogView log;

    // ---- 服务参数表单（停止时编辑；启动时固化进 config）----
    // 初始值来自命令行（parse_server_cli），所以"用参数启动"和"在界面里改"是同一套
    // 配置，不存在两套默认值。运行中整块禁用。
    char rpc_ip[64] = "0.0.0.0";
    char rpc_port[8] = "50051";
    // 数据面逐族开关：勾选框 = 是否服务该族（等价 CLI 的 --udp-ipv4/-ipv6 none）。
    // 地址框始终保留一个可编辑的默认值，取消勾选再勾回来不用重新输入。
    bool udp_ipv4_enabled = true;
    bool udp_ipv6_enabled = true;
    char udp_ipv4[64] = "0.0.0.0";
    char udp_ipv6[64] = "::";
    char udp_port[8] = "50000";
    // ---- 高级设置（音频格式 / 分帧）：平时不改，只在「高级设置」弹窗里编辑 ----
    bool follow_format = true;  // true = 用采集设备默认格式（config.format = nullopt）
    int sample_rate = 48000;
    int channels = 2;
    int encoding_index = 2;     // kEncodings 下标（2 = PCM F32LE）
    int packet_frames = 0;      // 每包帧数；0 = 自动
    int capture_source = 0;     // 0 = 系统输出（loopback）/ 1 = 输入设备
    bool pin_device = false;    // false = 跟随系统默认
    std::string pinned_device_id;
    std::string form_error;     // 启动前的本地校验失败原因（红字）

    // 采集设备下拉的数据源（停止时每 3s 刷新一次；运行中不枚举）。
    struct DeviceItem {
        std::string id;
        std::string name;
        bool is_default = false;
    };
    std::vector<DeviceItem> devices;

    // ---- 启动参数（CLI 解析结果；表单会覆盖其中由界面控制的字段）----
    aqua::runtime::ServerRuntimeConfig config;

    // ---- 运行态（每帧由 ServerSession::poll 写入；UI 只读）----
    ServerSnapshot snapshot;

    // ---- 窗口开关 ----
    bool show_settings = false; // 「设置」模态（外观 / 统计 / 高级）
    int settings_tab = -1;      // ServerSettingsTab 请求位；-1 = 无请求
    bool show_about = false;
    bool quit_requested = false;
    int page_request = -1; // ServerPage 之一；-1 = 无请求
};

// 状态文案（菜单 / 概况页 / 状态栏共用一套，避免三处各写一份 switch）。
const char* server_phase_label(ServerPhase phase);

} // namespace aqua::gui

#endif // AQUA_APP_GUI_SERVER_UI_STATE_H
