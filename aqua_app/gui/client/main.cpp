// aqua_client_gui：桌面图形客户端入口（Dear ImGui + SDL3）。
//
// 职责边界：本文件只做"窗口 + 渲染 + 主循环"；音频 / 传输逻辑直链
// aqua_client_core（与 CLI 同构——C API 的作用域是 Android，见 aqua_capi.h
// 头部注释）。跨平台：Windows / Linux / macOS 一套代码，后端 = SDL3 platform
// binding + SDLRenderer3 renderer binding（均编入 vcpkg 的 imgui 静态库）。
//
// 无头截图钩子（设计评审 / 文档用，正常路径不经过）：
//   AQUA_GUI_SCREENSHOT=<目录>  渲染 3 帧待布局稳定后，把 framebuffer 以 raw
//   RGBA 写入 <目录>/aqua_gui.raw（宽高写进 <目录>/aqua_gui.meta），再用
//   aqua_app/gui/tools/raw2png.py 转 PNG。无显示环境用法：
//     SDL_VIDEODRIVER=dummy AQUA_GUI_SCREENSHOT=out ./aqua_client_gui
//   AQUA_GUI_DEMO=1             注入拟真演示数据（不发起任何连接，仅预览界面）。
//   AQUA_GUI_OPEN_TAB=<名称>    启动即打开设置框并落在该 tab（截图用）：
//     外观 / 统计 / 高级（或 appearance / stats / advanced）；"关于"（about）
//     则直接打开关于窗口；空 / 未识别 = 不开。
//   AQUA_GUI_PAGE=<名称>        启动即切到该页面：连接 / 运行日志（或 connect / logs）。
//   AQUA_GUI_THEME=<名称>       启动主题：深色 / 浅色 / 经典（或 dark / light / classic）。
//     上面三个都只影响"启动时的那一下"，之后在界面里随便切。
//   AQUA_GUI_AUTOCONNECT=1      首帧自动发起连接（截图用）。配 SCREENSHOT 时
//     会等到真正连上、曲线采到点之后再截（连接是异步的，3 帧预热不够）。
//   AQUA_GUI_AUTODISCONNECT_MS=<毫秒>  连上之后再过这么久自动断开（截图"断开后"
//     的状态用：确认会话固定信息 / 曲线 / 指标都回到占位，而不是停在断线前）。
//   AQUA_GUI_SCREENSHOT_DELAY_MS=<毫秒> 预热之后再等这么久才落盘（带节流跑）：
//     帧率读数、曲线历史这类"要过几秒才有值"的东西靠它截到真实状态。
//   AQUA_GUI_FONT=<路径>|=none  覆盖 UI 字体（=none 跳过全部候选，用来验证"机器上
//     一个候选字体都没有"的降级路径：能开窗、中文变 '?'）。与 server 共用同一套
//     字体加载（common/app_window.cpp）。

#include "app_style.h"
#include "app_settings.h"
#include "client_session.h"
#include "log_buffer.h"
#include "log_sink.h"
#include "ui_main.h"
#include "ui_panels.h"

#include <SDL3/SDL.h>
#include <aqua/logger/logger.h>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlrenderer3.h>

#include "app_window.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

// 公共窗口层（common/app_window）：字体加载、降级路径、截图落盘都只有一份，
// client 与 server 共用——这里用 using 保持调用点不变。
using aqua::gui::env_enabled;
using aqua::gui::env_or_empty;
using aqua::gui::Fonts;
using aqua::gui::load_fonts;

// 默认窗口尺寸（逻辑点；SDL 建窗用物理像素 = 点 × 内容缩放）。
//
// 2026-10-18 二次改版：这是个小工具，不要宽屏。连接与运行日志改成同一个
// tab 条下的两页（整页切换），宽度回到 620——地址表 2.4:1:1.4 分列时端口框
// 仍有 ~100px，够放 "50051"。
// 高度 820 给"下半屏可视化"留空间：菜单 26 + tab 26 + 连接卡 ~320 +
// 遥测面板（指标网格 + 水位曲线 / 链路表两张并排卡 + 音频格式）~370 + 状态栏。
// 窗口可自由拉高，拉高时同时长大的是水位曲线和链路表的行高（行数固定 6，
// 行高 = 卡内高 / 6），其余各件的位置不动。
constexpr int kWindowWidth = 620;
constexpr int kWindowHeight = 820;

// 截图预热帧数：第 1 帧完成字体图集上传与布局，多渲染几帧避免截到半成品。
constexpr int kScreenshotWarmupFrames = 3;

// 自动连接截图时，连上之后还要再采这么多帧——水位曲线 500ms 一拍，少于 1 秒
// 的历史画出来只有一两个点（看起来像没数据）。
constexpr int kScreenshotSettleFrames = 150;
// 等连接建立的上限帧数（服务器不在时不要挂死）。
constexpr int kScreenshotWaitFrames = 600;
// 截图模式的硬上限帧数：超过就直接截，避免任何情况下窗口一直开着不退出。
constexpr int kScreenshotMaxFrames = 3000;

// AQUA_GUI_OPEN_TAB 的名称 → SettingsTab（未识别 = -1 = 不开设置框）。
int settings_tab_from_name(const std::string& name)
{
    if (name == "外观" || name == "appearance") {
        return aqua::gui::kSettingsAppearance;
    }
    if (name == "统计" || name == "stats") {
        return aqua::gui::kSettingsStats;
    }
    if (name == "高级" || name == "advanced") {
        return aqua::gui::kSettingsAdvanced;
    }
    if (name == "连接" || name == "connection") {
        return aqua::gui::kSettingsConnection;
    }
    return -1;
}

// AQUA_GUI_PAGE 的名称 → 页面下标（未识别 = -1 = 用默认页）。
int page_from_name(const std::string& name)
{
    if (name == "连接" || name == "connect") {
        return 0;
    }
    if (name == "运行日志" || name == "日志" || name == "logs" || name == "log") {
        return 1;
    }
    return -1;
}

// AQUA_GUI_THEME 的名称 → Theme（未识别 = 深色，与默认值一致）。
aqua::gui::Theme theme_from_name(const std::string& name)
{
    if (name == "浅色" || name == "light") {
        return aqua::gui::Theme::Light;
    }
    if (name == "经典" || name == "classic") {
        return aqua::gui::Theme::Classic;
    }
    return aqua::gui::Theme::Dark;
}

// 客户端名称的默认值：系统主机名。
//
// 为什么放在 GUI 侧而不是 core：`client_name` 是协议字段（服务端日志里用来识别
// "哪台机器在播"），但"默认填什么"是前端的选择。CLI 保持 aqua-client（脚本里
// 可预期），桌面端用主机名才符合直觉——两台机器连同一个服务器时，日志里一眼能
// 分出是谁。
//
// 取法：这一版 SDL3 没有主机名 API，用环境变量——Windows 的 COMPUTERNAME 每个
// 进程都有；POSIX 的 HOSTNAME 由 shell/登录会话设置，桌面自启动时可能为空，
// 那时退回默认值（连接仍然可用，只是名字不区分机器）。
//
// 移植规则：设置文件里存过自定义名字就沿用；空值、或旧版本的默认值
// "aqua-client"，一律视为"用户没选过"，换成主机名（否则老用户永远停在旧默认）。
// 取不到主机名就保留原值——空名会让 core 拒绝连接。
void apply_default_client_name(aqua::gui::UiState& s)
{
    const bool never_chosen = (s.client_name[0] == '\0'
        || std::strcmp(s.client_name, "aqua-client") == 0);
    if (!never_chosen) {
        return;
    }
    const char* host = SDL_getenv("COMPUTERNAME");
    if (host == nullptr || host[0] == '\0') {
        host = SDL_getenv("HOSTNAME");
    }
    if (host == nullptr || host[0] == '\0') {
        return;
    }
    std::string name = host;
    // core 的协议上限是 128 字节。截断不能把 UTF-8 字符切一半：尾部不完整时
    // 整段丢掉（主机名基本都是 ASCII，这里是兜底）。
    if (name.size() > 128) {
        name.resize(128);
    }
    std::size_t lead = name.size();
    while (lead > 0 && (static_cast<unsigned char>(name[lead - 1]) & 0xC0) == 0x80) {
        --lead;
    }
    if (lead > 0) {
        const unsigned char first = static_cast<unsigned char>(name[lead - 1]);
        const std::size_t need = (first >= 0xF0) ? 4 : (first >= 0xE0) ? 3 : (first >= 0xC0) ? 2 : 1;
        if (name.size() - (lead - 1) != need) {
            name.resize(lead - 1);
        }
    }
    if (name.empty()) {
        return;
    }
    std::snprintf(s.client_name, sizeof(s.client_name), "%s", name.c_str());
}

} // namespace

int main(int, char**)
{
    // 窗口程序没有自己的控制台：双击启动时这里是无操作（不会出现黑框）；从终端
    // 启动时把 stdout/stderr 接回父控制台，日志与报错在终端里照常可见。
    aqua::gui::attach_parent_console();

    // 日志：core 的 spdlog 输出先初始化，再把 GUI 的 sink 挂上去——此后 core
    // 的每一条日志都同时进控制台和运行日志面板（顺序不能反：init_logger 会
    // 整只替换默认 logger，先挂 sink 会被丢掉）。
    aqua::init_logger();
    aqua::gui::install_gui_log_sink();

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        // 窗口程序没有控制台：这里必须弹框，否则用户看到的就是"双击没反应"。
        aqua::gui::report_fatal(std::string("SDL 初始化失败：") + SDL_GetError());
        return 1;
    }

    // 建窗 / DPI / 可用区收紧 / 最小尺寸都在公共层（common/app_window.cpp）。
    aqua::gui::AppWindow app_window;
    aqua::gui::WindowOptions window_options;
    window_options.title = "Aqua Client";
    window_options.width = kWindowWidth;
    window_options.height = kWindowHeight;
    window_options.min_width = 600;
    window_options.min_height = 760;
    if (!aqua::gui::create_app_window(app_window, window_options)) {
        // 具体原因已经由 create_app_window 弹框 + 记日志了，这里只负责收尾。
        SDL_Quit();
        return 1;
    }
    SDL_Window* window = app_window.window;
    SDL_Renderer* renderer = app_window.renderer;
    const float main_scale = app_window.content_scale;

    // ImGui 上下文 + SDL3/SDLRenderer3 后端（失败时内部已回滚并弹框）。
    if (!aqua::gui::init_imgui(app_window)) {
        aqua::gui::destroy_app_window(app_window);
        SDL_Quit();
        return 1;
    }
    ImGuiIO& io = ImGui::GetIO();

    // 外观：主题（颜色 + 间距度量，含 DPI 缩放）+ 字体随 DPI 缩放。
    // UiState 先建：主题 / 字号 / DPI 都从它取，设置窗口的改动也写回它。
    aqua::gui::UiState ui;
    // 先读持久化设置：主题 / 字号这些必须赶在字体加载与 apply_theme 之前定下来，
    // 否则第一帧会先用默认外观渲染、再跳成存档里的外观。
    aqua::gui::load_settings(ui);
    // 客户端名称的默认值取系统主机名（GUI 侧；CLI 仍用 aqua-client）。
    // 规则：设置文件里存过自定义名字就沿用；空值或旧默认 "aqua-client" 视为
    // "没选过"，换成主机名——老用户升级后也能拿到有意义的名字。
    apply_default_client_name(ui);
    // 环境变量是"本次运行的显式意图"，优先级高于存档（截图钩子靠它钉死主题）。
    ui.demo = env_enabled("AQUA_GUI_DEMO");
    if (const std::string theme_override = env_or_empty("AQUA_GUI_THEME");
        !theme_override.empty()) {
        ui.theme = theme_from_name(theme_override);
    }
    ui.dpi_scale = main_scale;
    // 启动行：日志面板在连接前也有内容，顺便验证 core 日志 → 面板的桥是通的。
    // 走 core 的 spdlog（而不是直接写 LogBuffer），面板里这一行才和终端同格式。
    aqua::log_info(ui.demo ? "aqua_client_gui started (demo mode, no connection)"
                           : "aqua_client_gui started, waiting for connection");
    aqua::gui::apply_theme(ui.theme, main_scale, ui.density, ui.rounding);
    ImGuiStyle& style = ImGui::GetStyle();
    style.FontScaleDpi = main_scale; // 字体随 DPI 缩放（不重新烘焙图集）

    // 字体：UI 基准字号取 UiState 默认（16px；原生默认 13px 偏小，表单场景
    // 取更舒适的字号）；等宽 = 基准 × 0.875（日志 / 指标数值，与 UI 字号
    // 接近保证可读）。设置窗口改字号时不重载字体：1.92 按 size 惰性烘焙、
    // 纹理自动上传，主循环每帧写 style.FontSizeBase 即可（见下）。
    const Fonts fonts = load_fonts(static_cast<float>(ui.font_size),
        static_cast<float>(ui.font_size) * 0.875f);
    ui.font_mono = fonts.mono;

    const bool autoconnect = env_enabled("AQUA_GUI_AUTOCONNECT");
    const std::string screenshot_dir = env_or_empty("AQUA_GUI_SCREENSHOT");
    // 自动连接时把"连上之后的稳定帧"也算进预热：曲线要采够点才画得出形状。
    int screenshot_warmup = screenshot_dir.empty()
        ? 0
        : kScreenshotWarmupFrames + (autoconnect ? kScreenshotSettleFrames : 0);
    int screenshot_wait = 0; // 等连接建立的帧数（上限 kScreenshotWaitFrames）
    int screenshot_frames = 0; // 截图模式已经渲染的帧数（硬上限 kScreenshotMaxFrames）
    if (screenshot_warmup > 0) {
        SDL_CreateDirectory(screenshot_dir.c_str()); // 已存在时返回 false，无妨
    }
    // 预热之后再多等一段时间再截（AQUA_GUI_SCREENSHOT_DELAY_MS）：
    // 帧率读数、曲线历史这类"要过几秒才有值"的东西需要它；这段时间**带节流**跑，
    // 所以截到的帧率就是真实的静止帧率。
    const int screenshot_delay_ms = [] {
        const char* v = SDL_getenv("AQUA_GUI_SCREENSHOT_DELAY_MS");
        return v != nullptr ? std::atoi(v) : 0;
    }();
    Uint64 screenshot_release_ns = 0; // 0 = 预热还没结束
    // 截图辅助（见文件头注释）：启动即打开设置框的某个 tab / 切到某一页。
    const std::string open_tab_name = env_or_empty("AQUA_GUI_OPEN_TAB");
    if (open_tab_name == "关于" || open_tab_name == "about") {
        ui.show_about = true;
    } else {
        const int open_tab = settings_tab_from_name(open_tab_name);
        if (open_tab >= 0) {
            ui.settings_tab = open_tab;
            ui.show_settings = true;
        }
    }
    ui.page_request = page_from_name(env_or_empty("AQUA_GUI_PAGE"));

    // 截图辅助：连上后自动断开（见文件头）。0 = 不触发。
    const int autodisconnect_ms = [] {
        const char* v = SDL_getenv("AQUA_GUI_AUTODISCONNECT_MS");
        return v != nullptr ? std::atoi(v) : 0;
    }();
    bool autodisconnect_fired = false;

    bool done = false;
    // ---- 帧率节流 / 帧率读数的状态（规则见循环尾部）----
    // last_input_ms：最近一次收到输入事件的时刻；frame_start_ns：本帧开始时刻。
    // fps_window_*：0.5 s 滑窗，用来给设置→外观显示一个真实帧率。
    Uint64 last_input_ms = 0;
    Uint64 frame_start_ns = 0;
    Uint64 fps_window_start_ns = SDL_GetTicksNS();
    int fps_window_frames = 0;
    bool settings_open_last = false;
    Uint64 last_settings_save_ns = SDL_GetTicksNS();
    while (!done) {
        frame_start_ns = SDL_GetTicksNS();
        bool frame_had_input = false;
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            frame_had_input = true;
            ImGui_ImplSDL3_ProcessEvent(&event);
            if (event.type == SDL_EVENT_QUIT) {
                done = true;
            }
            if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED && event.window.windowID == SDL_GetWindowID(window)) {
                done = true;
            }
            // 无快捷键（用户 2026-10-18：Alt 不弹菜单栏、Tab 不导航控件，标注出来的
            // 快捷键反而像坏的）：菜单项全部鼠标操作，事件循环里不再截 Ctrl 组合键。
        }
        if (frame_had_input) {
            last_input_ms = SDL_GetTicks();
        }

        if (SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED) {
            SDL_Delay(10);
            continue;
        }

        // 字号：每帧把目标字号写进 FontSizeBase（ImGui 1.92 惰性烘焙 + 纹理
        // 自动上传，设置窗口拖动滑条即改，无需重载字体 / 重启）。
        // 除以 dpi_scale 让滑条读数 = 实际渲染像素（FontScaleDpi 会再乘回去）。
        style.FontSizeBase = static_cast<float>(ui.font_size) / main_scale;

        // 日志级别：设置里的「级别」同时驱动 core 的 spdlog 级别——面板只收到
        // 这一级以上的日志（否则选 Debug 也看不到 core 的 debug 行）。改了就
        // 下发，不用重启。
        static int applied_log_level = -1;
        // GUI 不提供 Trace：即使旧 ini 或别的入口写进 0，也夹到 Debug 再下发。
        const int gui_level = std::max(ui.log_min_level, aqua::gui::kGuiMinLogLevel);
        if (gui_level != applied_log_level) {
            applied_log_level = gui_level;
            using aqua::LogLevel;
            const LogLevel level = (gui_level == aqua::gui::kLogDebug) ? LogLevel::Debug
                : (gui_level == aqua::gui::kLogWarn)                   ? LogLevel::Warn
                : (gui_level == aqua::gui::kLogError)                  ? LogLevel::Error
                                                                       : LogLevel::Info;
            aqua::set_log_level(level);
        }

        // 日志行数上限：设置 → 外观 → 运行日志。改了就当场生效（缩小会裁掉最旧的
        // 行）；日志面板只在"运行日志"tab 可见时才画，所以不能放在面板里下发。
        static int applied_log_capacity = -1;
        if (ui.log_capacity != applied_log_capacity) {
            applied_log_capacity = ui.log_capacity;
            aqua::gui::log_buffer().set_capacity(
                static_cast<std::size_t>(std::max(200, ui.log_capacity)));
        }

        // 会话层 → UI：每帧取一次快照（互斥值拷贝，不阻塞）写进 UiState；
        // 刚连上时记一下时刻（状态栏 / 后续会话详情用）。
        const aqua::gui::SessionSnapshot session_snapshot = aqua::gui::client_session().poll();
        const bool just_connected = (session_snapshot.phase == aqua::gui::UiPhase::Connected
            && ui.phase != aqua::gui::UiPhase::Connected);
        aqua::gui::apply_session_snapshot(session_snapshot, ui);
        if (just_connected) {
            ui.connected_at = ImGui::GetTime();
        }

        // 截图辅助：首帧自动发起一次连接（AQUA_GUI_AUTOCONNECT=1）。
        static bool autoconnect_fired = false;
        if (autoconnect && !autoconnect_fired) {
            autoconnect_fired = true;
            aqua::gui::toggle_connection(ui);
        }
        // 截图辅助：连上后按 AQUA_GUI_AUTODISCONNECT_MS 自动断开一次（只触发一次）。
        if (autodisconnect_ms > 0 && ui.phase == aqua::gui::UiPhase::Connected
            && !autodisconnect_fired
            && ImGui::GetTime() - ui.connected_at >= static_cast<double>(autodisconnect_ms) / 1000.0) {
            autodisconnect_fired = true;
            aqua::gui::toggle_connection(ui);
        }

        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();

        aqua::gui::draw_main_window(ui);
        if (ui.quit_requested) {
            done = true; // 菜单栏 文件→退出（只置位，不覆盖事件循环的退出标记）
        }
        // 设置落盘：关框时立刻存一次；**框开着时每 2 s 也存一次**（debounce）——
        // 以前只在关窗/退出时写，改完主题直接崩掉就丢了。文件不到 1 KB，2 s 一次的
        // 写盘成本可以忽略，换来的是"随时崩都不会丢超过 2 秒的改动"。
        const Uint64 now_save_ns = SDL_GetTicksNS();
        if (settings_open_last && !ui.show_settings) {
            aqua::gui::save_settings(ui);
            last_settings_save_ns = now_save_ns;
        } else if (ui.show_settings && now_save_ns - last_settings_save_ns >= 2000000000ull) {
            aqua::gui::save_settings(ui);
            last_settings_save_ns = now_save_ns;
        }
        settings_open_last = ui.show_settings;

        ImGui::Render();
        SDL_SetRenderScale(renderer, io.DisplayFramebufferScale.x, io.DisplayFramebufferScale.y);
        // 清屏色跟随当前主题（设置窗口可实时切换主题）：每帧重取 WindowBg。
        const ImVec4 clear_color = ImGui::GetStyleColorVec4(ImGuiCol_WindowBg);
        SDL_SetRenderDrawColorFloat(renderer, clear_color.x, clear_color.y, clear_color.z, clear_color.w);
        SDL_RenderClear(renderer);
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
        SDL_RenderPresent(renderer);

        // 设备枚举（实测 5.6–8.6 ms / 6 个设备）放在 **Present 之后**：它吃的是
        // 本帧预算里的空闲尾巴（下面帧率节流那段的睡眠），不会把下一帧的绘制 /
        // 输入响应往后推。放在绘制之前时，那 8 ms 会直接叠在帧时间上——60 fps
        // 下就是每隔 5 秒抖一下。列表晚一帧生效，界面无感。
        aqua::gui::poll_audio_devices(ui);

        // 实测帧率（设置 → 外观显示）：0.5 s 滑窗，够快又不至于一直跳。
        ++fps_window_frames;        {
            const Uint64 now_ns = SDL_GetTicksNS();
            const Uint64 span_ns = now_ns - fps_window_start_ns;
            if (span_ns >= 500000000ull) {
                ui.render_fps = static_cast<float>(
                    static_cast<double>(fps_window_frames) * 1.0e9 / static_cast<double>(span_ns));
                fps_window_start_ns = now_ns;
                fps_window_frames = 0;
            }
        }

        if (screenshot_warmup > 0) {
            // 自动连接时先等连接真的建立（异步，3 帧预热远不够），再让曲线
            // 采够点；服务器不在时按帧数上限放弃，不至于挂死。
            if (autoconnect && ui.phase != aqua::gui::UiPhase::Connected) {
                ++screenshot_wait;
                if (screenshot_wait > kScreenshotWaitFrames) {
                    screenshot_warmup = 1; // 放弃等待：截一张未连接的
                }
            }
            --screenshot_warmup;
            // 兜底：帧数一路超过上限还没截到（曾经有次脚本卡住不退出），强制截一张。
            ++screenshot_frames;
            if (screenshot_warmup > 0 && screenshot_frames > kScreenshotMaxFrames) {
                screenshot_warmup = 1;
            }
        }
        if (!screenshot_dir.empty() && screenshot_warmup == 0 && !done) {
            // 预热结束 → 再多等 AQUA_GUI_SCREENSHOT_DELAY_MS（这段带节流），然后落盘。
            const Uint64 now_ns = SDL_GetTicksNS();
            if (screenshot_release_ns == 0) {
                screenshot_release_ns = now_ns
                    + static_cast<Uint64>(screenshot_delay_ms) * 1000000ull;
            }
            if (now_ns >= screenshot_release_ns) {
                aqua::gui::dump_framebuffer(renderer, screenshot_dir);
                done = true;
            }
        }

        // ---- 帧率节流 ----
        // 之前循环完全跟着显示器跑：这台机器是 180 Hz，于是稳定渲 180 fps——一个
        // "看数字"的面板白画了 3 倍于需要的帧（实测 3.3% 单核；裸 CLI 只有 0.2%）。
        // 目标帧率由设置 → 外观 →「刷新率」决定：
        //   自适应 = 有输入后 1.5 s 内 60 fps、静止 15 fps（默认）
        //   60 / 30 / 15 = 固定档；不限 = 不节流，只靠 vsync
        // 等待切成 ≤4 ms 的薄片，薄片里继续抽事件，但**只有"急"的事件才提前出帧**：
        //   · 点击 / 按键 / 滚轮 / 窗口事件 / 按住拖动 → 立刻出帧（跟手）；
        //   · 单纯移动鼠标 → 只续命"活跃窗口"，不打断等待。
        // 为什么后者重要：鼠标一动事件就是每几毫秒一个，见事件就 break 的话节流等于
        // 不存在——实测"自适应"档下鼠标划过页面会跑 100+ fps（用户 2026-10-19 反馈）。
        // 截图模式不节流：它按帧数预热 + 计数，节流会把 150 帧拖成十几秒。
        if (!done && screenshot_warmup <= 0) {
            int target_fps = 0; // 0 = 不节流
            switch (ui.frame_rate_mode) {
            case 1:  target_fps = 60; break;
            case 2:  target_fps = 30; break;
            case 3:  target_fps = 15; break;
            case 4:  target_fps = 0; break;
            case 0:
            default: target_fps = (SDL_GetTicks() - last_input_ms < 1500) ? 60 : 15; break;
            }
            // 自适应档且本帧判为"静止"：鼠标一动就缩短这一帧的等待（否则第一下移动
            // 还要等满 66 ms 才看到 hover 反馈），但不会因此突破 60 fps。
            const bool adaptive_idle = (ui.frame_rate_mode == 0 && target_fps == 15);
            if (target_fps > 0) {
                const Uint64 budget_ns = 1000000000ull / static_cast<Uint64>(target_fps);
                Uint64 deadline_ns = frame_start_ns + budget_ns;
                while (!done) {
                    const Uint64 now_ns = SDL_GetTicksNS();
                    if (now_ns >= deadline_ns) {
                        break;
                    }
                    const Uint64 left_ms = (deadline_ns - now_ns) / 1000000ull;
                    SDL_Delay(static_cast<Uint32>(std::clamp<Uint64>(left_ms, 1u, 4u)));
                    SDL_Event e;
                    bool any = false;
                    bool urgent = false;
                    bool motion = false;
                    while (SDL_PollEvent(&e)) {
                        any = true;
                        ImGui_ImplSDL3_ProcessEvent(&e);
                        switch (e.type) {
                        case SDL_EVENT_MOUSE_MOTION:
                            motion = true;
                            // 按住拖动（滑条 / 选文字 / 拖窗口）必须跟手。
                            urgent = urgent || (e.motion.state != 0);
                            break;
                        case SDL_EVENT_MOUSE_BUTTON_DOWN:
                        case SDL_EVENT_MOUSE_BUTTON_UP:
                        case SDL_EVENT_MOUSE_WHEEL:
                        case SDL_EVENT_KEY_DOWN:
                        case SDL_EVENT_KEY_UP:
                        case SDL_EVENT_TEXT_INPUT:
                            urgent = true;
                            break;
                        default:
                            if (e.type >= SDL_EVENT_WINDOW_FIRST && e.type <= SDL_EVENT_WINDOW_LAST) {
                                urgent = true; // 改变大小 / 曝光 / 失焦：要立即重画
                            }
                            break;
                        }
                        if (e.type == SDL_EVENT_QUIT) {
                            done = true;
                        }
                        if (e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED
                            && e.window.windowID == SDL_GetWindowID(window)) {
                            done = true;
                        }
                    }
                    if (any) {
                        last_input_ms = SDL_GetTicks();
                        if (adaptive_idle && motion) {
                            deadline_ns = std::min<Uint64>(deadline_ns, SDL_GetTicksNS() + 16000000ull);
                        }
                        if (urgent) {
                            break; // 急事：立刻出下一帧
                        }
                    }
                }
            }
        }
    }

    // 会话收尾：断开是异步的，给 worker 一点时间让 core 停干净并把尾部日志
    // 写进面板（最多等 2s，超时也不阻塞退出）。显式等待是为了让
    // client_session() 单例的析构（静态对象，时机不确定）落在日志系统仍可用
    // 的时候；shutdown_logger 只做排空，之后仍可安全打日志。
    aqua::gui::client_session().disconnect();
    for (int i = 0; i < 40 && aqua::gui::client_session().active(); ++i) {
        SDL_Delay(50);
    }

    // 退出时再存一次：兜住"设置框还开着就退出"的情况（关闭时已存过，重复无害）。
    aqua::gui::save_settings(ui);

    aqua::gui::shutdown_imgui(app_window);
    aqua::gui::destroy_app_window(app_window);
    SDL_Quit();
    aqua::shutdown_logger();
    return 0;
}
