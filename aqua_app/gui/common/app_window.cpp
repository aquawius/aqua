#include "app_window.h"

#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlrenderer3.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
// windows.h 默认定义 min/max 宏，会把下面的 std::min / std::max 全炸掉。
#define NOMINMAX
#endif
#include <windows.h> // AttachConsole / ATTACH_PARENT_PROCESS
// 兜底：若 windows.h 在别处已经被包含过（那时 NOMINMAX 还没定义），宏已经在了。
// 本文件到处是 std::min / std::max，这两个宏必须清掉。
#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif
#endif

namespace aqua::gui {
namespace {

// 中文字体候选（按优先级）。msyh.ttc 是 font collection——imgui 1.92 的 stb
// 前端支持（默认取第 0 个 face）。全部落空时用内置字体（仅拉丁字形）。
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

} // namespace

bool env_enabled(const char* name)
{
    const char* v = SDL_getenv(name);
    return v != nullptr && *v != '\0' && *v != '0';
}

std::string env_or_empty(const char* name)
{
    const char* v = SDL_getenv(name);
    return v != nullptr ? std::string(v) : std::string();
}

Fonts load_fonts(float ui_size, float mono_size)
{
    Fonts fonts;
    ImGuiIO& io = ImGui::GetIO();
    // 字号钳到可烘焙区间：0 / 负数会被 AddFontFromFileTTF 拒绝，NaN 会让整个
    // 图集尺寸算出垃圾值（PushFont(font, 0) 本身合法 = "沿用当前字号"，所以
    // 只要保证加载时是有限正数即可）。
    const auto sane = [](float v) {
        return (std::isfinite(v) && v >= 8.0f && v <= 48.0f) ? v : 16.0f;
    };
    ui_size = sane(ui_size);
    mono_size = sane(mono_size);

    // AQUA_GUI_FONT=<路径> 覆盖 UI 字体；=none 跳过全部候选（用来验证"机器上
    // 一个候选都没有"的降级路径，见下面 AddFontDefault）。
    const char* font_override = SDL_getenv("AQUA_GUI_FONT");
    const bool skip_candidates = font_override != nullptr
        && std::strcmp(font_override, "none") == 0;

    // ⚠️ 加载**必须**带 ImFontFlags_NoLoadError：AddFontFromFileTTF 在文件缺失时
    // 走 IM_ASSERT_USER_ERROR(0, "Could not load font file!")，而 ErrorLog() 返回
    // ConfigErrorRecoveryEnableAssert（默认 true）⇒ IM_ASSERT ⇒ assert() ⇒
    // **Debug 构建直接 abort**。
    // 候选表里第一个是 "C:\Windows\Fonts\msyh.ttc"（Windows 路径）——Linux /
    // macOS 上**必然缺失**，于是在没有该 flag 的 Debug 构建里，进程会在第一个候选
    // 就崩掉，压根走不到下面 AddFontDefault 的降级分支。
    // 该 flag 的语义正是"调用方自己检查返回值"——下面每个返回值都检查了。
    ImFontConfig load_cfg;
    load_cfg.Flags |= ImFontFlags_NoLoadError;

    if (font_override != nullptr && font_override[0] != '\0' && !skip_candidates) {
        fonts.ui = io.Fonts->AddFontFromFileTTF(font_override, ui_size, &load_cfg);
    }
    for (const char* path : kUiFontCandidates) {
        if (fonts.ui != nullptr || skip_candidates) {
            break;
        }
        fonts.ui = io.Fonts->AddFontFromFileTTF(path, ui_size, &load_cfg);
    }
    for (const char* path : kMonoFontCandidates) {
        if (skip_candidates) {
            break;
        }
        fonts.mono = io.Fonts->AddFontFromFileTTF(path, mono_size, &load_cfg);
        if (fonts.mono != nullptr) {
            break;
        }
    }
    if (fonts.mono != nullptr && fonts.mono != fonts.ui) {
        ImFontConfig merge_cfg;
        merge_cfg.MergeMode = true;
        // 合并进等宽字体图集：源字体缺失同样不能 assert。
        merge_cfg.Flags |= ImFontFlags_NoLoadError;
        for (const char* path : kUiFontCandidates) {
            if (io.Fonts->AddFontFromFileTTF(path, mono_size, &merge_cfg) != nullptr) {
                break;
            }
        }
    }
    // ---- 降级：一个候选都没有（精简 Linux / 容器 / 无中文字体的系统）----
    // 图集里**没有任何字体**时，后端建字体纹理那一步会失败/断言，窗口一片空白；
    // 而 PushFont(nullptr) 虽然合法（= 沿用当前字体），前提是当前字体存在。
    // 所以这里必须显式装内置 ProggyClean：界面变英文能看，但不会起不来。
    if (fonts.ui == nullptr) {
        fonts.ui = io.Fonts->AddFontDefault();
        SDL_Log("字体：未找到任何候选字体，退回 ImGui 内置字体（中文将显示为 '?'）");
    }
    if (fonts.mono == nullptr) {
        fonts.mono = fonts.ui; // 等宽降级为 UI 字体：日志可读，只是不对齐
    }
    return fonts;
}

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

void attach_parent_console()
{
#ifdef _WIN32
    // 已经有人给过 stdout / stderr 的真实句柄时**不要**接管：用户可能把输出重定向
    // 到了文件或管道（`aqua_server_gui --help > help.txt`），那种情况下 freopen 到
    // CONOUT$ 会把重定向抢走，文件里就空了。
    const HANDLE out_handle = ::GetStdHandle(STD_OUTPUT_HANDLE);
    const HANDLE err_handle = ::GetStdHandle(STD_ERROR_HANDLE);
    const bool need_out = (out_handle == nullptr || out_handle == INVALID_HANDLE_VALUE);
    const bool need_err = (err_handle == nullptr || err_handle == INVALID_HANDLE_VALUE);
    if (!need_out && !need_err) {
        return;
    }
    // 双击启动：没有父控制台 → AttachConsole 失败 → 什么都不做，也就不会像旧的
    // 控制台子系统那样弹一个黑框挂在后台。
    if (!::AttachConsole(ATTACH_PARENT_PROCESS)) {
        return;
    }
    // 接上之后 CRT 的流还是空的：重新打开到 CONOUT$ 才真正写进那个控制台。
    // （_CRT_SECURE_NO_WARNINGS 在 GUI 目标上已经定义，freopen 不会告警。）
    if (need_out) {
        (void)std::freopen("CONOUT$", "w", stdout);
    }
    if (need_err) {
        (void)std::freopen("CONOUT$", "w", stderr);
    }
#endif
    // 非 Windows：终端启动本来就有 stdout，不需要处理。
}

void report_fatal(const std::string& message)
{
    // 两个 GUI 都是**窗口程序**（CMake 里 WIN32_EXECUTABLE，见 aqua_app/CMakeLists.txt），
    // 没有控制台：SDL_Log / stdout 写的东西用户一个字也看不到。启动阶段的失败
    // （SDL_Init、建窗、渲染器、ImGui 后端）如果只写日志，表现出来就是"双击没反应"，
    // 所以这里统一"记日志 + 弹一个错误框"。
    // SDL_ShowSimpleMessageBox 在各平台都有原生实现（Windows = MessageBox，
    // Linux = GTK/zenity 或 X11 兜底），不需要平台 #ifdef。
    SDL_Log("%s", message.c_str());
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Aqua", message.c_str(), nullptr);
}

bool create_app_window(AppWindow& out, const WindowOptions& options)
{
    // DPI：内容缩放（1.0 = 96dpi）。窗口按物理像素建，ImGui 逻辑坐标 = 点。
    float main_scale = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
    if (!(main_scale > 0.0f)) {
        main_scale = 1.0f;
    }
    out.content_scale = main_scale;

    // 初始窗口尺寸按"显示器可用区 / 内容缩放"收紧：高 DPI 屏（125% / 150%
    // 缩放）的逻辑分辨率可能小于默认尺寸，不收紧窗口会超出屏幕。SDL 的 usable
    // bounds 是物理像素且**已扣除任务栏**，除以 main_scale 换算成逻辑点；余量只需
    // 再留原生标题栏（Win11 约 33px，随 DPI 缩放所以在点口径下近似恒定）+ 8px。
    int win_w = options.width;
    int win_h = options.height;
    SDL_Rect usable{};
    const bool have_usable = SDL_GetDisplayUsableBounds(SDL_GetPrimaryDisplay(), &usable);
    if (have_usable && main_scale > 0.0f) {
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
    out.window = SDL_CreateWindow(options.title,
        static_cast<int>(win_w * main_scale), static_cast<int>(win_h * main_scale), window_flags);
    if (out.window == nullptr) {
        // 个别平台 / 驱动（含无头 dummy 驱动）不认 HIGH_PIXEL_DENSITY，去掉再试
        // （该路径下 DisplayFramebufferScale = 1，逻辑尺寸 = 物理尺寸 = win_w/h）。
        window_flags &= ~SDL_WINDOW_HIGH_PIXEL_DENSITY;
        out.window = SDL_CreateWindow(options.title, win_w, win_h, window_flags);
    }
    if (out.window == nullptr) {
        report_fatal(std::string("无法创建窗口：") + SDL_GetError());
        return false;
    }

    out.renderer = SDL_CreateRenderer(out.window, nullptr);
    if (out.renderer == nullptr) {
        report_fatal(std::string("无法创建渲染器：") + SDL_GetError());
        SDL_DestroyWindow(out.window);
        out.window = nullptr;
        return false;
    }

    // 最小尺寸：再小下去内容会挤到显示不全（布局里有地板值，但那只是保底）。
    // 屏幕比这个下限还小时让步给屏幕：宁可挤一点，也不能让窗口比可用区还大。
    int min_w = options.min_width;
    int min_h = options.min_height;
    if (have_usable && main_scale > 0.0f) {
        const int avail_w = static_cast<int>(static_cast<float>(usable.w) / main_scale) - 8;
        const int avail_h = static_cast<int>(static_cast<float>(usable.h) / main_scale) - 8;
        min_w = std::min(min_w, std::max(avail_w, 320));
        min_h = std::min(min_h, std::max(avail_h, 240));
    }
    SDL_SetWindowMinimumSize(out.window,
        static_cast<int>(min_w * main_scale), static_cast<int>(min_h * main_scale));
    SDL_SetRenderVSync(out.renderer, 1); // 无头软件渲染下失败也无所谓
    SDL_SetWindowPosition(out.window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    SDL_ShowWindow(out.window);
    return true;
}

bool init_imgui(AppWindow& w)
{
    IMGUI_CHECKVERSION();
    if (ImGui::CreateContext() == nullptr) {
        report_fatal("ImGui 上下文创建失败（内存不足）");
        return false;
    }
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr; // 不落 imgui.ini：布局由代码决定，保持环境干净
    // 不开 ImGuiConfigFlags_NavEnableKeyboard：它承诺的 Alt 激活菜单栏 / Tab 导航
    // 实际不工作（2026-10-18 用户反馈），菜单项一律鼠标操作。

    const bool sdl3_backend_ok = ImGui_ImplSDL3_InitForSDLRenderer(w.window, w.renderer);
    const bool renderer_backend_ok = sdl3_backend_ok && ImGui_ImplSDLRenderer3_Init(w.renderer);
    if (!renderer_backend_ok) {
        report_fatal("ImGui 后端初始化失败（SDL3 / SDLRenderer3）");
        if (sdl3_backend_ok) {
            ImGui_ImplSDL3_Shutdown();
        }
        ImGui::DestroyContext();
        return false;
    }
    return true;
}

void shutdown_imgui(AppWindow&)
{
    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
}

void destroy_app_window(AppWindow& w)
{
    if (w.renderer != nullptr) {
        SDL_DestroyRenderer(w.renderer);
        w.renderer = nullptr;
    }
    if (w.window != nullptr) {
        SDL_DestroyWindow(w.window);
        w.window = nullptr;
    }
}

} // namespace aqua::gui
