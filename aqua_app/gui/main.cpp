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

#include "app_style.h"
#include "ui_main.h"

#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlrenderer3.h>

#include <cstdio>
#include <string>

namespace {

// 默认窗口尺寸（逻辑点；SDL 建窗用物理像素 = 点 × 内容缩放）。
// 取值依据用户 2026-10-16 截图：整窗 621×690 物理 px（100% DPI 原生捕获，
// 文字墨高 11~12px = 16px 字号 @100%，125%/150% 均被排除），减去原生标题栏
// 33px 得客户区 620×657；宽度严格取 620。高度取 700（比截图客户区多 43px）：
// 上下分屏后连接卡片 393px + 日志半区（tab + 工具栏 + ~3.5 行日志）合计需要
// 约 625px 内容区；1366×768 屏上会被 usable bounds 收紧到 687，依然放得下。
// 窗口可自由拉高，日志区自动变大。旧值 960×780 被用户评为"宽度有点过宽"。
// 小屏 / 高 DPI 仍由下方 usable bounds 收紧兜底。
constexpr int kWindowWidth = 620;
constexpr int kWindowHeight = 700;

// 截图预热帧数：第 1 帧完成字体图集上传与布局，多渲染几帧避免截到半成品。
constexpr int kScreenshotWarmupFrames = 3;

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
    SDL_SetRenderVSync(renderer, 1); // 无头软件渲染下失败也无所谓
    SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    SDL_ShowWindow(window);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr; // 不落 imgui.ini：v1 无持久化，保持环境干净
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    // 外观：主题（颜色 + 间距度量，含 DPI 缩放）+ 字体随 DPI 缩放。
    // UiState 先建：主题 / 字号 / DPI 都从它取，设置窗口的改动也写回它。
    aqua::gui::UiState ui;
    ui.demo = env_enabled("AQUA_GUI_DEMO");
    ui.dpi_scale = main_scale;
    aqua::gui::apply_theme(ui.theme, main_scale);
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

    const std::string screenshot_dir = env_or_empty("AQUA_GUI_SCREENSHOT");
    int screenshot_warmup = screenshot_dir.empty() ? 0 : kScreenshotWarmupFrames;
    if (screenshot_warmup > 0) {
        SDL_CreateDirectory(screenshot_dir.c_str()); // 已存在时返回 false，无妨
    }

    bool done = false;
    while (!done) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL3_ProcessEvent(&event);
            if (event.type == SDL_EVENT_QUIT) {
                done = true;
            }
            if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED && event.window.windowID == SDL_GetWindowID(window)) {
                done = true;
            }
            // 菜单快捷键：Ctrl+K 连接/断开、Ctrl+P 外观设置。带 Ctrl 的按键
            // 不会被 ImGui 文本输入消费，直通安全。
            // 上下分屏后运行日志常驻可见，原 Ctrl+L（切到日志页）取消；
            // Ctrl+K 从"切到连接页"改为真正的连接/断开（与文件菜单标注一致）。
            if (event.type == SDL_EVENT_KEY_DOWN && (event.key.mod & SDL_KMOD_CTRL) != 0) {
                if (event.key.key == SDLK_K) {
                    aqua::gui::toggle_connection(ui);
                } else if (event.key.key == SDLK_P) {
                    ui.show_settings = true;
                }
            }
        }

        if (SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED) {
            SDL_Delay(10);
            continue;
        }

        // 字号：每帧把目标字号写进 FontSizeBase（ImGui 1.92 惰性烘焙 + 纹理
        // 自动上传，设置窗口拖动滑条即改，无需重载字体 / 重启）。
        // 除以 dpi_scale 让滑条读数 = 实际渲染像素（FontScaleDpi 会再乘回去）。
        style.FontSizeBase = static_cast<float>(ui.font_size) / main_scale;

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
            --screenshot_warmup;
            if (screenshot_warmup == 0) {
                dump_framebuffer(renderer, screenshot_dir);
                done = true;
            }
        }
    }

    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
