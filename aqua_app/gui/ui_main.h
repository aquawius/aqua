#ifndef AQUA_APP_GUI_UI_MAIN_H
#define AQUA_APP_GUI_UI_MAIN_H

// 主窗口 UI：菜单栏 + 上下分屏主体（上：连接卡片；下：运行日志）+ 底部状态栏
// + 设置等附加窗口。布局参照 ImHex：菜单栏在顶、主体常驻、重要信息在状态栏。
//
// 分层约定：本头只声明纯数据状态（UiState）与绘制入口，所有 ImGui 调用都在
// ui_main.cpp。会话（ClientRuntime）接入后，由独立 session 层填充 UiState 的
// 会话字段；UI 线程不直接触碰 core 对象。

#include "app_style.h"

#include <imgui.h>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace aqua::gui {

// 日志级别（数值大可过滤，与 spdlog 的级别排序一致）。
enum LogLevel {
    kLogDebug = 0,
    kLogInfo,
    kLogWarn,
    kLogError,
};

// 一行运行日志。
struct LogLine {
    std::string stamp; // HH:MM:SS（本地时间）
    int level = kLogInfo;
    std::string text;
};

// 线程安全的日志环形缓冲（容量有界，旧行挤出）。
// Phase 0 只有 UI 事件；core 的 spdlog sink 在会话接入后写进同一缓冲。
class LogBuffer {
public:
    void append(int level, std::string text);
    void clear();
    [[nodiscard]] std::vector<LogLine> snapshot(int min_level) const;
    [[nodiscard]] bool save_to_file(const std::string& path) const;
    // 增 / 清时自增，供 UI 做增量刷新（避免每帧拷贝全部行）。
    [[nodiscard]] std::uint64_t version() const;

private:
    static constexpr std::size_t kCapacity = 2000;
    mutable std::mutex mutex_;
    std::deque<LogLine> lines_;
    std::uint64_t version_ = 0;
};

// 会话阶段。Phase 0 只有 Idle 与 Connected（后者仅 AQUA_GUI_DEMO 演示可达）；
// Connecting / Disconnecting 预留给 session 状态机。
enum class UiPhase {
    Idle,
    Connecting,
    Connected,
    Disconnecting,
};

// 主窗口全部可变状态。v1 不持久化——每次启动取默认值。
struct UiState {
    // 环境开关
    bool demo = false;           // AQUA_GUI_DEMO=1：拟真演示数据（不发起连接）
    ImFont* font_mono = nullptr; // 等宽字体（日志 / 指标区）；空 = 回退默认字体

    // 外观（设置窗口实时切换；main.cpp 每帧应用，ImGui 1.92 惰性烘焙即时生效）
    Theme theme = Theme::Dark;   // 主题（深色 / 浅色 / 经典）
    int font_size = 16;         // UI 基准字号（px，12–24；等宽 = font_size * 0.875）
    float dpi_scale = 1.0f;      // 显示内容缩放，main.cpp 启动时写入

    // 连接设置（Phase 0 仅内存；会话接入时映射到 ClientRuntimeConfig）
    char rpc_ip[64] = "127.0.0.1"; // gRPC 服务器地址
    char rpc_port[8] = "50051";    // gRPC 端口
    char udp_port[8] = "";         // 空 = 采用 Server 通告（CLI 的 --udp-force-port）
    int device_index = 0;          // 播放设备下标；0 = 系统默认（跟随）
    bool pin_device = false;       // true = 钉住所选设备；false = 跟随系统默认
    int preset_index = 2;          // 网络预设下标（见 ui_main.cpp 预设表；2 = Wi-Fi）
    bool custom_config = false;    // 自定义配置：高级参数里的自定义 JB 配置优先生效
    bool auto_reconnect = true;    // 断线自动重连（UI 在设置窗口；会话接入后生效）

    // 会话状态（session 层驱动；Phase 0 仅演示按钮切换）
    UiPhase phase = UiPhase::Idle;
    double demo_connected_at = 0.0; // 演示连接起始时刻（ImGui 时间轴）

    // 附加窗口开关
    bool show_diagnostics = false;
    bool show_advanced = false;
    bool show_settings = false;
    bool show_about = false;

    // 生命周期：菜单栏 文件→退出 置位，main 循环读取后结束。
    bool quit_requested = false;

    // 日志面板视图设置（工具栏与设置窗口共用）
    int log_min_level = kLogInfo;
    bool log_autoscroll = true;
    int log_font_size = 14;       // 日志输出框字号（px，10–20；工具栏滑条调，独立于 UI 字号）
};

// 绘制全屏主窗口（菜单栏 + 上下分屏 + 状态栏 + 附加窗口）。
void draw_main_window(UiState& state);

// 连接 / 断开（演示模式切换 phase；会话接入后由 session 层驱动）。
// 菜单 文件→连接/断开、连接大按钮、Ctrl+K 快捷键三者共用同一入口。
void toggle_connection(UiState& state);

// 全局日志缓冲（会话接入后 spdlog sink 也写它）。
LogBuffer& log_buffer();

} // namespace aqua::gui

#endif // AQUA_APP_GUI_UI_MAIN_H
