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

// 刷新率模式（设置 → 外观）：主动期的帧率 + 静止期的帧率。
//   0 自适应：有输入时 60 fps，静止 1.5 s 后 15 fps（默认，最省电）
//   1 60 fps / 2 30 fps / 3 15 fps：固定值，不再区分动静
//   4 不限：只靠 vsync（高刷屏上会跑满刷新率，最耗电）
inline constexpr int kFrameRateModeCount = 5;
inline constexpr const char* kFrameRateModeNames[kFrameRateModeCount] = {
    "自适应（60 / 15）", "固定 60 fps", "固定 30 fps", "固定 15 fps", "不限（跟随显示器）",
};

// 会话阶段（ClientSession 的状态机经 apply_session_snapshot 每帧写入；
// 演示模式 AQUA_GUI_DEMO 下由 UI 自维护，不消费会话快照）。
enum class UiPhase {
    Idle,
    Connecting,
    Connected,
    Disconnecting,
    Failed, // 连接失败 / 会话异常终止（status_detail 里是原因；可再次点连接）
    // 自动重连的"等待窗口"（退避中，还没发起下一次 start）。与 Connecting 分开是
    // **必须**的：2026-10-19 之前重连期间 phase 一直是 Connecting，而 UI 的
    // "连接中"意味着按钮禁用 + 菜单禁用 + 参数锁定 ⇒ 用户既停不下来也改不了地址，
    // 只能杀进程。现在这个阶段按钮是「取消重连」。
    Reconnecting,
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

    // ---- 会话固定信息（连接成功时由会话层写入，**断开时由会话层清空**）----
    // 空 / 0 = 当前没有会话：UI 一律显示占位（"—" / "-"），不显示上一场会话的残留。
    std::uint64_t session_id = 0;
    std::string data_plane;     // "ip:port"；空 = 未通告
    std::string audio_format;   // 一行摘要："48 kHz · 立体声 · PCM F32LE · 3072 kbps · 175 帧 ≈ 3.6 ms"
    int sample_rate = 0;
    int channels = 0;
    double rx_rate = 0.0; // UDP 音频包到达速率（包/秒；会话层算好，UI 只显示）
    // 音频格式的分项（设置→统计的「音频格式」分节用；与 audio_format 同一来源，
    // 免得 UI 去反解析那句人话摘要）。
    std::string encoding;    // "PCM F32LE"
    int bits_per_sample = 0; // 32
    int bitrate_kbps = 0;    // 3072
    int frame_count = 0;     // 每包帧数 F
    double packet_ms = 0.0;  // 包时长（F / 采样率）
    // 快照发布序号：遥测面板的趋势缓冲只在它变化时采一个点（否则按帧采样，
    // 60fps 下 60 秒历史两秒就填满）。
    std::uint64_t snapshot_seq = 0;

    // ---- 第二轮诊断扩充（core 2026-10-19；与 ClientDiagnosticsSnapshot 末尾追加组同序）----
    // UI 不推演、只搬运；单位规则同 core：frames / slots / ns，ms 在边界派生。
    struct DiagEx {
        // 输出流：引擎周期与格式协商
        std::uint32_t engine_period_frames = 0;      // 选定的引擎周期
        std::uint32_t engine_period_min_frames = 0;  // 0 = 未上报
        std::uint32_t engine_period_max_frames = 0;
        std::uint32_t engine_period_default_frames = 0; // 引擎默认周期
        std::uint32_t engine_period_fundamental_frames = 0; // 引擎基本周期（粒度单位）
        bool engine_resampling = false;              // OS SRC 在线路里
        std::uint32_t mix_sample_rate = 0;           // 端点混音格式
        std::uint32_t mix_channels = 0;
        std::uint32_t buffer_size_frames = 0;       // 后端实际缓冲帧数
        std::uint32_t output_buffering_frames = 0;  // 此刻压在输出缓冲里的帧数
        // playback 消费侧补充
        std::uint64_t stale_events_dropped = 0;      // 播放事件过期被丢弃
        std::uint64_t empty_event_skips = 0;         // WASAPI 的 underrun 信号
        std::uint32_t catchup_frames = 0;            // 上次事件时可写帧数
        std::uint32_t catchup_frames_max = 0;
        std::uint32_t callback_frames = 0;           // 最近一次回调请求帧数
        bool sample_rate_adjusted = false;           // 系统 SRC 改过采样率
        // 网络：退避 / 错误分类 / 观测分母 / 对端身份
        std::uint32_t rx_consecutive_errors = 0;     // >0 = 正在退避
        std::int32_t rx_last_error_code = 0;
        std::uint64_t estimator_packets = 0;         // 丢包率分母
        std::uint64_t estimator_gap_events = 0;
        std::uint64_t estimator_missing_packets = 0;
        bool estimator_base_set = false;
        bool udp_address_from_sentinel = false;
        std::int32_t udp_family = 0;                 // 0=未知 4=IPv4 6=IPv6
        std::string learned_peer_endpoint;           // 实际对端 "ip:port"（观测值）
        // 抖动缓冲：第三项序列 + 开关可见性
        std::uint64_t oldest_received_sequence = 0;
        bool conceal_enabled = false;
        std::uint32_t conceal_max_slots = 0;
        bool splice_enabled = false;
        std::uint32_t splice_xfade_frames = 0;
        std::uint32_t startup_slots = 0;
        std::uint32_t initial_target_slots = 0;
        // 决策层补充
        std::uint32_t effective_min_slots = 0;
        bool in_storm = false;
        double last_stall_margin_slots = 0.0;
        double stall_peak_cap_slots = 0.0;
        double fall_rate_slots_per_sec = 0.0;
        // 远端协商格式
        std::uint32_t remote_sample_rate = 0;
        std::uint32_t remote_channels = 0;
        std::int32_t remote_encoding = 0;
        std::uint32_t remote_bitrate_kbps = 0;
        std::uint32_t remote_frame_count = 0;
        // ---- 2026-10-20：回调开销 / 端点时钟 / reanchor 代价 ----
        std::uint64_t callback_cost_ns = 0;
        std::uint64_t callback_cost_max_ns = 0;
        std::uint64_t callback_overruns = 0;
        std::uint64_t endpoint_latency_ns = 0;
        std::uint64_t clock_position_ticks = 0;
        std::uint64_t clock_frequency_hz = 0;
        std::uint64_t reanchor_dropped_slots = 0;
        std::uint32_t reanchor_dropped_last = 0;
        std::int32_t reanchor_reason = 0;
    } diag_ex;

    // 完整诊断快照的透传（设置→统计的详表用）。live=false 时内容是上次会话
    // 的残留或全零，**不得显示**——统计页以 live 为门。UiState 不解释字段，
    // 只负责搬运；字段口径见 client_diagnostics_snapshot.h。
    aqua::diagnostics::ClientDiagnosticsSnapshot diag {};
};

// 设置窗口的 tab。当作"请求位"用：菜单项 / 卡片「高级参数…」按钮置位后打开
// 设置框，对应 tab 一旦被真正选中就清回 -1（避免每帧 SetSelected 把用户手动
// 切走的 tab 又拽回来——这个坑第 9 轮踩过）。
enum SettingsTab {
    kSettingsAppearance = 0, // 外观（主题 / 密度 / 圆角 / 字号 / 日志视图）
    kSettingsConnection,     // 连接（自动重连及其间隔）
    kSettingsStats,          // 统计（实时指标 + 全量诊断明细）
    kSettingsAdvanced,       // 高级（抖动缓冲参数）
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
    Theme theme = Theme::Dark;     // 主题（深色 / 浅色 / 经典）
    Density density = Density::Normal; // 界面密度（紧凑 / 标准 / 宽松）
    float rounding = 5.0f;          // 圆角像素（默认 5 = ImGui 原生观感；0 = 方角，0–10）
    int font_size = 16;            // UI 基准字号（px，12–24；指标条等宽 = ×0.875）
    float dpi_scale = 1.0f;        // 显示内容缩放，main.cpp 启动时写入
    int frame_rate_mode = 0;       // 刷新率模式（见 kFrameRateModeNames；持久化）
    float render_fps = 0.0f;       // 实测帧率（设置→外观的读数；运行期值，不持久化）

    // ---- 连接设置（会话层读取 → ClientRuntimeConfig）----
    char rpc_ip[64] = "127.0.0.1"; // gRPC 服务器地址
    char rpc_port[8] = "50051";    // gRPC 端口
    char udp_port[8] = "";         // 空 = 采用 Server 通告（CLI 的 --udp-force-port）
    // 客户端名称（CLI 的 --client-name）：服务端 Connect 会硬拒空名与 >128 字节的
    // 名字，所以缓冲区长 129（输入框天然截到 128），空值在 build_session_config 里
    // 被拦下并给出原因。
    char client_name[129] = "aqua-client";
    int device_index = 0;          // 播放设备下标；0 = 系统默认（跟随）
    bool pin_device = false;       // true = 钉住所选设备；false = 跟随系统默认
    int preset_index = 2;          // 网络预设下标（见 jb_presets.cpp 预设表；2 = Wi-Fi）
    bool custom_config = false;    // 自定义配置：jb_custom 优先生效，预设滑条置灰
    JbParams jb_custom;            // 自定义 JB 原始值（0 = core 默认，与 Android 滑条同语义）
    // ---- 其余连接属性（设置 → 高级；CLI 早有对应开关，GUI 之前没暴露）----
    // 全部在建连那一刻固化，运行期不可切——与上面 6 个 JB 参数同一套锁定规则。
    // 注：不暴露 playback.low_latency——它只有 AAudio（Android）后端读取，桌面
    // 后端忽略，放个开关在桌面上等于假开关。桌面端真正管延迟的是「回放缓冲」。
    bool jb_adaptive_target = true;  // 自适应 target（关 = 固定 0.60N/0.50N，--jb-fixed-target）
    bool jb_concealment = true;      // PCM 掩盖（关 = 缺帧直接静音，--jb-no-conceal）
    bool jb_splice = true;           // 拼接 crossfade（关 = 硬拼接，--jb-no-splice）
    bool jb_packet_trace = false;    // 逐包 trace 日志（--jb-trace；需日志级别 Trace）
    int playback_buffer_frames = 0;  // 回放设备 I/O 粒度（帧；0 = 设备允许的最小周期）
    bool auto_reconnect = true;    // 断线自动重连（设置 → 连接；会话层读取）
    int reconnect_delay_s = 3;     // 自动重连间隔秒数（1–10；会话层读取）

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
    int log_font_size = 14;       // 日志输出框字号（px，10–20；设置里调，独立于 UI 字号）
    int log_capacity = 2000;      // 日志缓冲行数上限（200–10000；设置里调，持久化）
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
