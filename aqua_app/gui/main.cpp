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

#include "app_style.h"
#include "client_session.h"
#include "log_buffer.h"
#include "log_sink.h"
#include "ui_main.h"

#include <SDL3/SDL.h>
#include <aqua/logger/logger.h>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlrenderer3.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

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

// 中文字体候选（按优先级）。msyh.ttc 是 font collection——imgui 1.92 的 stb
// 前端支持（默认取第 0 个 face）。全部落空时 ImGui 用内置默认字体（仅拉丁
// 字形，中文为方框——真机上至少有一个候选存在）。
constexpr const char* kUiFontCandidates[] = {
    "C:\\Windows\\Fonts\\msyh.ttc",
    "C:\\Windows\\Fonts\\Deng.ttf",
    "C:\\Windows\\Fonts\\simhei.ttf",
    "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
    "/usr/share/fonts/truetype/wqy/wqy-microhei.ttc",
    "/System/Library/Fonts/PingFang.ttc",
    "/System/Library/Fonts/Supplemental/Songti.ttc",
};

// 等宽字体（日志面板）。与 UI 字体分开加载：日志区 PushFont 使用。
// 注意：等宽字体不含中文字形，加载后必须把中文字体合并进来（见 load_fonts）。
constexpr const char* kMonoFontCandidates[] = {
    "C:\\Windows\\Fonts\\consola.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
    "/System/Library/Fonts/Menlo.ttc",
};

bool env_enabled(const char* name)
{
    const char* v = SDL_getenv(name);
    return v != nullptr && *v != '\0' && *v != '0';
}

const char* env_or_empty(const char* name)
{
    const char* v = SDL_getenv(name);
    return v != nullptr ? v : "";
}

// 把 framebuffer 读回并写成 raw RGBA（行主序、无行填充）+ meta（宽 高）。
// 转 PNG 用 aqua_app/gui/tools/raw2png.py（纯 zlib，无第三方依赖）。
void dump_framebuffer(SDL_Renderer* renderer, const std::string& dir)
{
    SDL_Surface* shot = SDL_RenderReadPixels(renderer, nullptr);
    if (shot == nullptr) {
        SDL_Log("screenshot: SDL_RenderReadPixels 失败: %s", SDL_GetError());
        return;
    }
    SDL_Surface* rgba = SDL_ConvertSurface(shot, SDL_PIXELFORMAT_RGBA32);
    SDL_DestroySurface(shot);
    if (rgba == nullptr) {
        SDL_Log("screenshot: SDL_ConvertSurface 失败: %s", SDL_GetError());
        return;
    }

    const std::string raw_path = dir + "/aqua_gui.raw";
    const std::string meta_path = dir + "/aqua_gui.meta";
    bool ok = false;
    if (std::FILE* f = std::fopen(raw_path.c_str(), "wb"); f != nullptr) {
        const int row_bytes = rgba->w * 4;
        ok = true;
        for (int y = 0; y < rgba->h; ++y) {
            const auto* row = static_cast<const Uint8*>(rgba->pixels) + static_cast<std::size_t>(y) * rgba->pitch;
            if (std::fwrite(row, 1, static_cast<std::size_t>(row_bytes), f) != static_cast<std::size_t>(row_bytes)) {
                ok = false;
                break;
            }
        }
        std::fclose(f);
    }
    if (ok) {
        if (std::FILE* m = std::fopen(meta_path.c_str(), "w"); m != nullptr) {
            std::fprintf(m, "%d %d\n", rgba->w, rgba->h);
            std::fclose(m);
        }
        SDL_Log("screenshot: 已写入 %s（%dx%d）", raw_path.c_str(), rgba->w, rgba->h);
    } else {
        SDL_Log("screenshot: 写入 %s 失败", raw_path.c_str());
    }
    SDL_DestroySurface(rgba);
}

struct Fonts {
    ImFont* ui = nullptr;   // UI 主字体（中文优先）
    ImFont* mono = nullptr; // 等宽（日志面板）
};

// 加载 UI 主字体（中文优先）与等宽字体（日志区）。
// 等宽字体（Consolas / DejaVu Sans Mono）不含中文字形：把 UI 字体以
// MergeMode 合并进等宽字体图集（合并目标 = 图集中最后一个字体 = mono），
// 否则日志里的中文会被渲染成 '?'。
Fonts load_fonts(float ui_size, float mono_size)
{
    Fonts fonts;
    ImGuiIO& io = ImGui::GetIO();
    for (const char* path : kUiFontCandidates) {
        fonts.ui = io.Fonts->AddFontFromFileTTF(path, ui_size);
        if (fonts.ui != nullptr) {
            break;
        }
    }
    for (const char* path : kMonoFontCandidates) {
        fonts.mono = io.Fonts->AddFontFromFileTTF(path, mono_size);
        if (fonts.mono != nullptr) {
            break;
        }
    }
    if (fonts.mono != nullptr) {
        ImFontConfig merge_cfg;
        merge_cfg.MergeMode = true;
        for (const char* path : kUiFontCandidates) {
            if (io.Fonts->AddFontFromFileTTF(path, mono_size, &merge_cfg) != nullptr) {
                break;
            }
        }
    }
    return fonts;
}

} // namespace

int main(int, char**)
{
    // 日志：core 的 spdlog 输出先初始化，再把 GUI 的 sink 挂上去——此后 core
    // 的每一条日志都同时进控制台和运行日志面板（顺序不能反：init_logger 会
    // 整只替换默认 logger，先挂 sink 会被丢掉）。
    aqua::init_logger();
    aqua::gui::install_gui_log_sink();

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        SDL_Log("SDL_Init 失败: %s", SDL_GetError());
        return 1;
    }

    // DPI：内容缩放（1.0 = 96dpi）。窗口按物理像素建，ImGui 逻辑坐标 = 点。
    float main_scale = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
    if (!(main_scale > 0.0f)) {
        main_scale = 1.0f;
    }

    // 初始窗口尺寸按"显示器可用区 / 内容缩放"收紧：高 DPI 屏（125% / 150%
    // 缩放）的逻辑分辨率可能小于默认尺寸，不收紧窗口会超出屏幕——用户看到
    // "窗口太大、有东西显示在外面"。SDL 的 usable bounds 是物理像素且**已
    // 扣除任务栏**，除以 main_scale 换算成逻辑点；余量只需再留原生标题栏
    // （Win11 约 33px，随 DPI 缩放所以在点口径下近似恒定）+ 8px 安全边。
    // （曾经的 -56 把任务栏扣了两次，白送 15px 高度。）
    int win_w = kWindowWidth;
    int win_h = kWindowHeight;
    SDL_Rect usable{};
    if (SDL_GetDisplayUsableBounds(SDL_GetPrimaryDisplay(), &usable) && main_scale > 0.0f) {
        const int avail_w = static_cast<int>(static_cast<float>(usable.w) / main_scale) - 40;
        const int avail_h = static_cast<int>(static_cast<float>(usable.h) / main_scale) - 41;
        if (win_w > avail_w && avail_w > 320) {
            win_w = avail_w;
        }
        if (win_h > avail_h && avail_h > 240) {
            win_h = avail_h;
        }
    }

    SDL_WindowFlags window_flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_HIDDEN;
    SDL_Window* window = SDL_CreateWindow("Aqua Client",
        static_cast<int>(win_w * main_scale), static_cast<int>(win_h * main_scale), window_flags);
    if (window == nullptr) {
        // 个别平台 / 驱动（含无头 dummy 驱动）不认 HIGH_PIXEL_DENSITY，去掉再试
        // （该路径下 DisplayFramebufferScale = 1，逻辑尺寸 = 物理尺寸 = win_w/h）。
        window_flags &= ~SDL_WINDOW_HIGH_PIXEL_DENSITY;
        window = SDL_CreateWindow("Aqua Client", win_w, win_h, window_flags);
    }
    if (window == nullptr) {
        SDL_Log("SDL_CreateWindow 失败: %s", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    SDL_Renderer* renderer = SDL_CreateRenderer(window, nullptr);
    if (renderer == nullptr) {
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
    // 最小尺寸：再小下去连接卡片会挤成两行、遥测面板的曲线会被压成一条缝（布局里
    // 虽然有地板值，但那只是保底，不算"能看"）。600×760 是各区块刚好排得下的下限
    // ——比它小就由 SDL 挡住，用户不会拖出一个"显示不全"的窗口。
    // 屏幕比这个下限还小（小笔记本 / 大缩放）时让步给屏幕：宁可挤一点，也不能让
    // 窗口比可用区还大（那会把底部状态栏挤到屏幕外）。
    int min_w = 600;
    int min_h = 760;
    if (usable.w > 0 && usable.h > 0 && main_scale > 0.0f) {
        const int avail_w = static_cast<int>(static_cast<float>(usable.w) / main_scale) - 8;
        const int avail_h = static_cast<int>(static_cast<float>(usable.h) / main_scale) - 8;
        min_w = std::min(min_w, std::max(avail_w, 320));
        min_h = std::min(min_h, std::max(avail_h, 240));
    }
    SDL_SetWindowMinimumSize(window, static_cast<int>(min_w * main_scale), static_cast<int>(min_h * main_scale));
    SDL_SetRenderVSync(renderer, 1); // 无头软件渲染下失败也无所谓
    SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    SDL_ShowWindow(window);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr; // 不落 imgui.ini：v1 无持久化，保持环境干净
    // 不开 ImGuiConfigFlags_NavEnableKeyboard：用户 2026-10-18 反馈"按 Alt 弹不出
    // 菜单栏、Tab 也切不到控件"——这个 flag 承诺的是方向键 / Tab 导航与 Alt 激活
    // 菜单栏，实际两者都不工作（后者还需要平台层的键位配合）。承诺了却做不到的
    // "快捷键"不如没有：菜单项一律鼠标操作，不做标注。

    // 外观：主题（颜色 + 间距度量，含 DPI 缩放）+ 字体随 DPI 缩放。
    // UiState 先建：主题 / 字号 / DPI 都从它取，设置窗口的改动也写回它。
    aqua::gui::UiState ui;
    ui.demo = env_enabled("AQUA_GUI_DEMO");
    ui.theme = theme_from_name(env_or_empty("AQUA_GUI_THEME"));
    ui.dpi_scale = main_scale;
    // 启动行：日志面板在连接前也有内容，顺便验证 core 日志 → 面板的桥是通的。
    // 走 core 的 spdlog（而不是直接写 LogBuffer），面板里这一行才和终端同格式。
    aqua::log_info(ui.demo ? "aqua_client_gui started (demo mode, no connection)"
                           : "aqua_client_gui started, waiting for connection");
    aqua::gui::apply_theme(ui.theme, main_scale, ui.density, ui.rounding);
    ImGuiStyle& style = ImGui::GetStyle();
    style.FontScaleDpi = main_scale; // 字体随 DPI 缩放（不重新烘焙图集）

    const bool sdl3_backend_ok = ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
    const bool renderer_backend_ok = sdl3_backend_ok && ImGui_ImplSDLRenderer3_Init(renderer);
    if (!renderer_backend_ok) {
        SDL_Log("ImGui backend 初始化失败");
        if (sdl3_backend_ok) {
            ImGui_ImplSDL3_Shutdown();
        }
        ImGui::DestroyContext();
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

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
    // ---- 帧率节流的状态（规则见循环尾部）----
    // last_input_ms：最近一次收到输入事件的时刻；frame_start_ms：本帧开始时刻。
    Uint64 last_input_ms = 0;
    Uint64 frame_start_ms = 0;
    while (!done) {
        frame_start_ms = SDL_GetTicks();
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
        if (ui.log_min_level != applied_log_level) {
            applied_log_level = ui.log_min_level;
            using aqua::LogLevel;
            const LogLevel level = (ui.log_min_level == aqua::gui::kLogTrace) ? LogLevel::Trace
                : (ui.log_min_level == aqua::gui::kLogDebug)                  ? LogLevel::Debug
                : (ui.log_min_level == aqua::gui::kLogWarn)                   ? LogLevel::Warn
                : (ui.log_min_level == aqua::gui::kLogError)                  ? LogLevel::Error
                                                                              : LogLevel::Info;
            aqua::set_log_level(level);
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

        ImGui::Render();
        SDL_SetRenderScale(renderer, io.DisplayFramebufferScale.x, io.DisplayFramebufferScale.y);
        // 清屏色跟随当前主题（设置窗口可实时切换主题）：每帧重取 WindowBg。
        const ImVec4 clear_color = ImGui::GetStyleColorVec4(ImGuiCol_WindowBg);
        SDL_SetRenderDrawColorFloat(renderer, clear_color.x, clear_color.y, clear_color.z, clear_color.w);
        SDL_RenderClear(renderer);
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
        SDL_RenderPresent(renderer);

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
            if (screenshot_warmup == 0) {
                dump_framebuffer(renderer, screenshot_dir);
                done = true;
            }
        }

        // ---- 帧率节流 ----
        // 之前循环完全跟着显示器跑：这台机器是 180 Hz，于是稳定渲 180 fps——一个
        // "看数字"的面板白画了 3 倍于需要的帧（实测 3.3% 单核；裸 CLI 只有 0.2%）。
        // 规则：
        //   · 最近 1.5 s 内有输入（鼠标 / 键盘 / 滚轮 / 窗口事件）→ 60 fps；
        //   · 静止时 15 fps：曲线 500 ms 一拍、运行时长 1 s 一跳，完全够用；
        //   · 等待切成 ≤4 ms 的薄片，薄片里继续抽事件——静止时点按钮的响应仍是
        //     亚帧级（不会等满 66 ms），窗口关闭也立刻被看到；
        //   · 截图模式不节流：它按帧数预热 + 计数，节流会把 150 帧拖成十几秒。
        if (!done && screenshot_warmup <= 0) {
            const Uint64 now_ms = SDL_GetTicks();
            const bool active = (now_ms - last_input_ms) < 1500;
            const Uint64 budget_ms = active ? 16u : 66u; // 60 fps / 15 fps
            const Uint64 deadline = frame_start_ms + budget_ms;
            while (!done && SDL_GetTicks() < deadline) {
                const Uint64 left = deadline - SDL_GetTicks();
                SDL_Delay(static_cast<Uint32>(std::min<Uint64>(left, 4u)));
                SDL_Event e;
                bool woke = false;
                while (SDL_PollEvent(&e)) {
                    woke = true;
                    ImGui_ImplSDL3_ProcessEvent(&e);
                    if (e.type == SDL_EVENT_QUIT) {
                        done = true;
                    }
                    if (e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED
                        && e.window.windowID == SDL_GetWindowID(window)) {
                        done = true;
                    }
                }
                if (woke) {
                    last_input_ms = SDL_GetTicks();
                    break; // 有事件：立刻出下一帧，输入不排队
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

    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    aqua::shutdown_logger();
    return 0;
}
