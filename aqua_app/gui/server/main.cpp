// aqua_server_gui：桌面图形服务端入口（Dear ImGui + SDL3）。
//
// 与 aqua_server_cli 的关系：**参数解析完全复用 CLI 的那一份**（parse_server_cli），
// 所以 --rpc-ip / --udp-port / --format / --capture-device … 的行为一字不差，
// 不存在"GUI 有一套自己的默认值"这种漂移。命令行不传就是 CLI 的默认值。
//
// 职责边界：本文件只做"启动 + 主循环 + 收尾"；服务端生命周期在 server_session
// （worker 线程 + 500ms 诊断快照），界面在 ui_server。
//
// 无头截图钩子（与 client 同一套环境变量）：
//   AQUA_GUI_SCREENSHOT=<目录>        渲染数帧后把 framebuffer 写成 raw RGBA
//                                     （再用 aqua_app/gui/tools/raw2png.py 转 PNG）。
//   AQUA_GUI_SCREENSHOT_DELAY_MS=<毫秒> 预热后再等这么久才落盘（让速率有值）。
//   AQUA_GUI_AUTOSTART=1              按命令行参数直接启动服务（截图 / 脚本用；
//                                     正常启动是"先填参数、再点按钮"）。
//   AQUA_GUI_PAGE=服务|overview|日志|logs       启动即切到该页。
//   AQUA_GUI_PAGE=统计|stats|外观|高级          启动即打开设置窗口并落在该 tab。
//   AQUA_GUI_PAGE=关于|about                    启动即打开关于窗。
//   AQUA_GUI_THEME=深色|浅色|经典       启动主题（覆盖存档）。
//   AQUA_GUI_FONT=<路径>|=none        覆盖 UI 字体 / 复现"无字体"降级路径。

#include "app_style.h"
#include "app_window.h"
#include "log_sink.h"
#include "server_session.h"
#include "server_settings.h"
#include "server_ui_state.h"
#include "ui_format.h"
#include "ui_server.h"

#include "cli_parser/cli_parser_server.h"

#include <aqua/logger/logger.h>

#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlrenderer3.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

// 620×780：宽与 client 同宽（两个前端并排摆在桌面上时左缘 / 右缘对齐，切窗口不会
// 觉得换了套东西）。
// 高度 780：三张卡（参数 303 + 运行信息 191 + 会话 160）正好排下、页面不出滚动条，
// 会话卡能显示 3 行客户端。再矮就整页滚动（会话卡本身仍有 3 行的最小高度）。
constexpr int kWindowWidth = 620;
constexpr int kWindowHeight = 780;

int page_from_name(const std::string& name)
{
    if (name == "概况" || name == "服务" || name == "overview" || name == "status"
        || name == "service") {
        return aqua::gui::kPageService;
    }
    if (name == "运行日志" || name == "日志" || name == "logs" || name == "log") {
        return aqua::gui::kPageLog;
    }
    return -1;
}

// 「统计」不再是页面 tab，而是设置窗口的一个 tab：截图/脚本入口直接把它打开。
bool settings_tab_from_name(const std::string& name, int& tab)
{
    if (name == "统计" || name == "stats" || name == "statistics") {
        tab = aqua::gui::kSettingsStats;
        return true;
    }
    if (name == "外观" || name == "appearance") {
        tab = aqua::gui::kSettingsAppearance;
        return true;
    }
    if (name == "高级" || name == "advanced") {
        tab = aqua::gui::kSettingsAdvanced;
        return true;
    }
    return false;
}

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

} // namespace

int main(int argc, char** argv)
{
    // 日志排空必须最后执行（逆序析构）：core 的 worker 都 join 之后才关 sink。
    struct LogDrain {
        ~LogDrain() { aqua::shutdown_logger(); }
    };
    const LogDrain log_drain;

    // 窗口程序没有自己的控制台：双击启动时这里是无操作（不会出现黑框）；从终端
    // 启动时把 stdout/stderr 接回父控制台，--help / --version / 日志才看得见。
    // 必须在参数解析之前——解析器把 usage 打进 stdout。
    aqua::gui::attach_parent_console();
    aqua::cli::configure_console_utf8();

    aqua::runtime::ServerRuntimeConfig config;
    aqua::LogLevel log_level = aqua::default_log_level();
    std::string log_file;
    if (const auto exit_code = aqua::cli::cli_exit_code(
            aqua::cli::parse_server_cli(argc, argv, config, log_level, log_file))) {
        return *exit_code;
    }

    aqua::init_logger(log_file);
    aqua::set_log_level(log_level);
    aqua::gui::install_gui_log_sink();
    aqua::log_info("aqua_server_gui started");

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        // 窗口程序没有控制台：这里必须弹框，否则用户看到的就是"双击没反应"。
        aqua::gui::report_fatal(std::string("SDL 初始化失败：") + SDL_GetError());
        return 1;
    }

    aqua::gui::AppWindow app_window;
    aqua::gui::WindowOptions window_options;
    window_options.title = "Aqua Server";
    window_options.width = kWindowWidth;
    window_options.height = kWindowHeight;
    window_options.min_width = 560;
    window_options.min_height = 420;
    if (!aqua::gui::create_app_window(app_window, window_options)) {
        // 具体原因已经由 create_app_window 弹框 + 记日志了，这里只负责收尾。
        SDL_Quit();
        return 1;
    }
    SDL_Window* window = app_window.window;
    SDL_Renderer* renderer = app_window.renderer;
    const float main_scale = app_window.content_scale;

    if (!aqua::gui::init_imgui(app_window)) {
        aqua::gui::destroy_app_window(app_window);
        SDL_Quit();
        return 1;
    }
    ImGuiIO& io = ImGui::GetIO();

    using aqua::gui::env_enabled;
    using aqua::gui::env_or_empty;

    aqua::gui::ServerUiState ui;
    ui.config = config; // CLI 的其它参数（超时/队列/advertise）作为表单的底座
    // 命令行解析结果先填进表单（地址/端口/格式/采集设备在界面里就是它们），
    // 再读存档：外观照单全收；表单字段只在"命令行没显式传"时沿用上次的值
    // （见 server_settings.h 的优先级说明——--udp-port 50100 不能被存档改掉）。
    aqua::gui::load_form_from_config(ui, config);
    ui.dpi_scale = main_scale;
    aqua::gui::load_server_settings(ui, config);
    if (const std::string theme_override = env_or_empty("AQUA_GUI_THEME");
        !theme_override.empty()) {
        ui.theme = theme_from_name(theme_override);
    }
    ui.page_request = page_from_name(env_or_empty("AQUA_GUI_PAGE"));
    // "关于"（about）直接开关于窗；其余名字落到设置框的某个 tab（与 client 一致）。
    const std::string open_page = env_or_empty("AQUA_GUI_PAGE");
    if (open_page == "关于" || open_page == "about") {
        ui.show_about = true;
    } else if (int tab = -1; settings_tab_from_name(open_page, tab)) {
        ui.settings_tab = tab;
        ui.show_settings = true;
    }

    // 日志面板的前端差异只有三处：导出目录 / 文件名前缀 / 空列表文案。
    ui.log.export_dir = aqua::gui::server_data_dir();
    ui.log.export_prefix = "aqua-server";
    ui.log.empty_hint = "（暂无日志：服务端启动后这里会实时输出 core 的日志）";

    aqua::gui::apply_theme(ui.theme, main_scale, ui.density, ui.rounding);
    ImGuiStyle& style = ImGui::GetStyle();
    style.FontScaleDpi = main_scale;

    const aqua::gui::Fonts fonts = aqua::gui::load_fonts(
        static_cast<float>(ui.font_size), static_cast<float>(ui.font_size) * aqua::gui::kMonoRatio);
    ui.font_mono = fonts.mono;
    ui.log.mono_font = fonts.mono;

    // 默认**不**自动启动：和 client 一样"先填参数、再点按钮"，否则表单从第一帧
    // 起就是禁用状态，用户根本看不到地址框是可编辑的。
    // 脚本 / 截图用 AQUA_GUI_AUTOSTART=1 按命令行参数直接启动。
    if (env_enabled("AQUA_GUI_AUTOSTART")) {
        aqua::gui::toggle_server(ui);
    }

    const std::string screenshot_dir = env_or_empty("AQUA_GUI_SCREENSHOT");
    const int screenshot_delay_ms = [] {
        const char* v = SDL_getenv("AQUA_GUI_SCREENSHOT_DELAY_MS");
        return v != nullptr ? std::atoi(v) : 0;
    }();
    int screenshot_warmup = screenshot_dir.empty() ? 0 : 3;
    Uint64 screenshot_release_ns = 0;
    if (!screenshot_dir.empty()) {
        SDL_CreateDirectory(screenshot_dir.c_str()); // 已存在时返回 false，无妨
    }

    bool done = false;
    Uint64 last_input_ms = 0;
    Uint64 frame_start_ns = 0;
    // 实测帧率：最近 0.5 秒渲染了多少帧（设置 → 外观里显示，用来判断刷新率是否生效）。
    Uint64 fps_window_start_ns = SDL_GetTicksNS();
    int fps_window_frames = 0;

    // 主题 / 密度 / 圆角 / 字号 / 日志级别：只在变化时下发。
    aqua::gui::Theme applied_theme = ui.theme;
    aqua::gui::Density applied_density = ui.density;
    float applied_rounding = ui.rounding;
    int applied_log_level = -1;

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
            if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED
                && event.window.windowID == SDL_GetWindowID(window)) {
                done = true;
            }
        }
        if (frame_had_input) {
            last_input_ms = SDL_GetTicks();
        }
        if (SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED) {
            SDL_Delay(10);
            continue;
        }

        if (ui.theme != applied_theme || ui.density != applied_density
            || ui.rounding != applied_rounding) {
            applied_theme = ui.theme;
            applied_density = ui.density;
            applied_rounding = ui.rounding;
            aqua::gui::apply_theme(ui.theme, main_scale, ui.density, ui.rounding);
            style.FontScaleDpi = main_scale;
        }
        style.FontSizeBase = static_cast<float>(ui.font_size) / main_scale;

        if (ui.log.min_level != applied_log_level) {
            applied_log_level = ui.log.min_level;
            using aqua::LogLevel;
            const LogLevel level = (ui.log.min_level == aqua::gui::kLogTrace) ? LogLevel::Trace
                : (ui.log.min_level == aqua::gui::kLogDebug)                  ? LogLevel::Debug
                : (ui.log.min_level == aqua::gui::kLogWarn)                   ? LogLevel::Warn
                : (ui.log.min_level == aqua::gui::kLogError)                  ? LogLevel::Error
                                                                              : LogLevel::Info;
            aqua::set_log_level(level);
        }

        ui.snapshot = aqua::gui::ServerSession::instance().poll();
        if (ui.quit_requested) {
            done = true;
        }

        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();

        aqua::gui::draw_server_window(ui);

        ImGui::Render();
        SDL_SetRenderScale(renderer, io.DisplayFramebufferScale.x, io.DisplayFramebufferScale.y);
        const ImVec4 clear_color = ImGui::GetStyleColorVec4(ImGuiCol_WindowBg);
        SDL_SetRenderDrawColorFloat(renderer, clear_color.x, clear_color.y, clear_color.z, clear_color.w);
        SDL_RenderClear(renderer);
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
        SDL_RenderPresent(renderer);

        ++fps_window_frames;
        {
            const Uint64 now_ns = SDL_GetTicksNS();
            const Uint64 span_ns = now_ns - fps_window_start_ns;
            if (span_ns >= 500000000ull) {
                ui.render_fps = static_cast<float>(
                    static_cast<double>(fps_window_frames) * 1.0e9 / static_cast<double>(span_ns));
                fps_window_start_ns = now_ns;
                fps_window_frames = 0;
            }
        }

        // ---- 无头截图 ----
        if (!screenshot_dir.empty() && !done) {
            if (screenshot_warmup > 0) {
                --screenshot_warmup;
            } else {
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
        }

        // ---- 帧率节流（与服务端无关：这只是个看数字的窗口）----
        if (!done && screenshot_warmup <= 0) {
            int target_fps = 0;
            switch (ui.frame_rate_mode) {
            case 1:  target_fps = 60; break;
            case 2:  target_fps = 30; break;
            case 3:  target_fps = 15; break;
            case 4:  target_fps = 0; break;
            case 0:
            default: target_fps = (SDL_GetTicks() - last_input_ms < 1500) ? 60 : 15; break;
            }
            if (target_fps > 0) {
                const Uint64 deadline_ns = frame_start_ns
                    + 1000000000ull / static_cast<Uint64>(target_fps);
                while (!done) {
                    const Uint64 now_ns = SDL_GetTicksNS();
                    if (now_ns >= deadline_ns) {
                        break;
                    }
                    const Uint64 left_ms = (deadline_ns - now_ns) / 1000000ull;
                    SDL_Delay(static_cast<Uint32>(std::clamp<Uint64>(left_ms, 1u, 4u)));
                    SDL_Event e;
                    bool urgent = false;
                    while (SDL_PollEvent(&e)) {
                        ImGui_ImplSDL3_ProcessEvent(&e);
                        // 等待期间到达的任何事件都算"用户还在动"：自适应档据此回到
                        // 60 fps。之前只在外层 poll 里记录输入，等待期间到达的鼠标
                        // 移动被这里吞掉 ⇒ 一直动鼠标却始终停在 15 fps（用户反馈
                        // "刷新率看起来不生效"就是这条）。固定档不受影响：这里只
                        // 记录时间戳，不打断等待（break 仍只由 urgent 触发）。
                        last_input_ms = SDL_GetTicks();
                        if (e.type == SDL_EVENT_QUIT) {
                            done = true;
                        }
                        if (e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED
                            && e.window.windowID == SDL_GetWindowID(window)) {
                            done = true;
                        }
                        // 单纯移动鼠标不打断等待（否则节流等于不存在）；点击 / 键
                        // 盘 / 滚轮 / 窗口事件要立刻重画。
                        if (e.type != SDL_EVENT_MOUSE_MOTION) {
                            urgent = true;
                        }
                    }
                    if (urgent) {
                        break;
                    }
                }
            }
        }
    }

    // 收尾：先请求停止，等 worker 把 core 的线程 join 掉（最多 5s），再关窗口。
    aqua::gui::ServerSession::instance().stop();
    aqua::gui::ServerSession::instance().wait_for_stop(5000);
    aqua::gui::save_server_settings(ui);

    aqua::gui::shutdown_imgui(app_window);
    aqua::gui::destroy_app_window(app_window);
    SDL_Quit();
    return 0;
}
