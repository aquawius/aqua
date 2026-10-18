#ifndef AQUA_APP_GUI_UI_STATE_H
#define AQUA_APP_GUI_UI_STATE_H

// 主窗口全部可变状态（纯数据：无 ImGui 调用、无业务逻辑）。
// 各面板的派生缓存（设备列表 / 日志快照 / 布局实测值）刻意不放进来——
// 它们在各自 .cpp 的 *PanelState 结构里，丢重置即可，见 ui_panels.h 说明。

#include "app_style.h"
#include "jb_presets.h"

#include <aqua/diagnostics/client_diagnostics_snapshot.h>

#include <imgui.h>

#include <cstdint>
#include <string>

// log_min_level 的默认值用 LogLevel 枚举。
#include "log_buffer.h"

namespace aqua::gui {

// 会话阶段（ClientSession 的状态机经 apply_session_snapshot 每帧写入；
// 演示模式 AQUA_GUI_DEMO 下由 UI 自维护，不消费会话快照）。
enum class UiPhase {
    Idle,
    Connecting,
    Connected,
    Disconnecting,
    Failed, // 连接失败 / 会话异常终止（status_detail 里是原因；可再次点连接）
};

// 会话实时数据视图（ClientSession::poll → apply_session_snapshot 每帧写入；
// UI 只读）。没有实时会话时 live=false，指标一律显示 "-"，绝不残留上一次的数字。
struct SessionData {
    bool live = false;
    double water_level = 0.0;    // 0..1（缓冲水位）
    double lead_ms = 0.0;        // 缓冲领先播放的时间
    double target_ms = 0.0;      // 自适应目标
    double jitter_ms = 0.0;      // 到达抖动估计（EWMA）
    double gap_ratio = 0.0;      // 接收序列缺口率（乱序与丢包都计入）
    double underrun_ratio = 0.0; // 欠载帧占比
    std::string status_detail;   // 最近一次状态变化 / 错误（人话）
    int reconnect_attempt = 0;   // >0 = 正在第 N 次自动重连

    // ---- 会话固定信息（连接成功时由会话层写入；断开后保留，供遥测面板展示）----
    std::uint64_t session_id = 0;
    std::string data_plane;     // "ip:port"；空 = 未通告
    std::string audio_format;   // "48 kHz · 立体声 · PCM F32LE · 32 bit · 1536 kbps · 480 帧 ≈ 10.0 ms"
    int sample_rate = 0;
    int channels = 0;
    // 快照发布序号：遥测面板的趋势缓冲只在它变化时采一个点（否则按帧采样，
    // 60fps 下 60 秒历史两秒就填满）。
    std::uint64_t snapshot_seq = 0;

    // 完整诊断快照的透传（设置→统计的详表用）。live=false 时内容是上次会话
    // 的残留或全零，**不得显示**——统计页以 live 为门。UiState 不解释字段，
    // 只负责搬运；字段口径见 client_diagnostics_snapshot.h。
    aqua::diagnostics::ClientDiagnosticsSnapshot diag {};
};

// 设置窗口的 tab。当作"请求位"用：菜单项 / 卡片「高级参数…」按钮置位后打开
// 设置框，对应 tab 一旦被真正选中就清回 -1（避免每帧 SetSelected 把用户手动
// 切走的 tab 又拽回来——这个坑第 9 轮踩过）。
enum SettingsTab {
    kSettingsAppearance = 0, // 外观
    kSettingsStats,          // 统计（原"诊断面板"）
    kSettingsAdvanced,       // 高级（网络预设高级参数等）
};

// 主窗口全部可变状态。v1 不持久化——每次启动取默认值。
//
// 接线约定（会话层接入时看这里，分三类）：
//   ·「连接设置」段：用户输入，会话层**读取**（映射到 ClientRuntimeConfig）；
//   ·「会话状态」段：会话层**写入**，UI 只读 + 展示，UI 不推演；
//   · 其余段（环境 / 外观 / 窗口开关 / 生命周期 / 日志视图）：UI 自有，
//     会话层不碰。
struct UiState {
    // ---- 环境开关 ----
    bool demo = false;           // AQUA_GUI_DEMO=1：拟真演示数据（不发起连接）
    ImFont* font_mono = nullptr; // 等宽字体（日志 / 指标区）；空 = 回退默认字体

    // ---- 外观（设置窗口实时切换；main.cpp 每帧应用，ImGui 1.92 惰性烘焙即时生效）----
    Theme theme = Theme::Dark;   // 主题（深色 / 浅色 / 经典）
    int font_size = 16;          // UI 基准字号（px，12–24；指标条等宽 = ×0.875）
    float dpi_scale = 1.0f;      // 显示内容缩放，main.cpp 启动时写入

    // ---- 连接设置（会话层读取 → ClientRuntimeConfig）----
    char rpc_ip[64] = "127.0.0.1"; // gRPC 服务器地址
    char rpc_port[8] = "50051";    // gRPC 端口
    char udp_port[8] = "";         // 空 = 采用 Server 通告（CLI 的 --udp-force-port）
    int device_index = 0;          // 播放设备下标；0 = 系统默认（跟随）
    bool pin_device = false;       // true = 钉住所选设备；false = 跟随系统默认
    int preset_index = 2;          // 网络预设下标（见 jb_presets.cpp 预设表；2 = Wi-Fi）
    bool custom_config = false;    // 自定义配置：jb_custom 优先生效，预设滑条置灰
    JbParams jb_custom;            // 自定义 JB 原始值（0 = core 默认，与 Android 滑条同语义）
    bool auto_reconnect = true;    // 断线自动重连（设置窗口里；会话层读取）

    // ---- 会话状态（会话层写入；UI 只读 + 展示）----
    UiPhase phase = UiPhase::Idle;
    double connected_at = 0.0;     // 连接建立时刻（ImGui 时间轴；演示模式自维护）
    SessionData session;           // 实时指标 / 状态详情（每帧由会话层快照写入）
    // 上一次连接动作被本地挡下的原因（端口非法 / core 端点校验失败）。每次点
    // 连接或断开时清空；卡片里显示，不参与会话状态机。
    std::string action_error;

    // ---- 附加窗口开关（菜单栏 / 卡片按钮置位）----
    bool show_settings = false;       // 统一设置框（外观 / 统计 / 高级 三个 tab）
    int settings_tab = -1;            // 请求位：SettingsTab 之一，tab 生效后清回 -1
    bool show_about = false;

    // ---- 生命周期 ----
    // 菜单栏 文件→退出 置位，main 循环读取后结束。
    bool quit_requested = false;

    // 页面切换请求位（0 = 连接 / 1 = 运行日志；-1 = 无请求）。
    // 顶层 tab 是即时模式的"点出来的"状态，没有可以被外部写入的字段；需要定向
    // 打开某一页时（AQUA_GUI_PAGE 截图钩子，将来做"打开日志页"的深链接）置位，
    // 由 draw_page_tabs 消费一次后清回 -1——同 settings_tab 的请求位语义。
    int page_request = -1;

    // ---- 日志面板视图设置（工具栏与设置窗口共用）----
    int log_min_level = kLogInfo;
    bool log_autoscroll = true;
    int log_font_size = 12;       // 日志输出框字号（px，10–20；设置里调，独立于 UI 字号）
};

// 开 / 关「自定义配置」的唯一入口（连接卡片的勾选框与设置→高级都走它）。
// 开启那一刻把当前预设档位解析出的值抄进 jb_custom——起点 = 用户刚看过的
// 档位，而不是一组毫不相干的 core 默认；关闭不回写预设（档位没被动过）。
inline void set_custom_config(UiState& s, bool on)
{
    if (on && !s.custom_config) {
        s.jb_custom = jb_resolve(s.preset_index);
    }
    s.custom_config = on;
}

} // namespace aqua::gui

#endif // AQUA_APP_GUI_UI_STATE_H
