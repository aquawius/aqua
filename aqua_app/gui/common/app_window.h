#ifndef AQUA_APP_GUI_APP_WINDOW_H
#define AQUA_APP_GUI_APP_WINDOW_H

// 桌面窗口引导：SDL3 建窗 + ImGui 上下文 + 字体 + 截图落盘。
//
// client 与 server 两个前端共用这一份——"同一套外观"从这里开始：
// 主题由 app_style 管，字体候选 / DPI / 降级路径 / 截图钩子都在本文件里只有一份，
// 不会出现"client 有中文字体、server 没有"这种漂移。
//
// 本文件不碰任何业务：创建窗口、初始化后端、绘制帧之间的胶水都不在这里。

#include <imgui.h>
#include <SDL3/SDL.h>

#include <string>

namespace aqua::gui {

struct Fonts {
    ImFont* ui = nullptr;   // UI 主字体（中文优先）
    ImFont* mono = nullptr; // 等宽（日志 / 数值）
};

// SDL 版环境变量读取（不用 std::getenv：Windows 下 SDL 会做 UTF-8 转换）。
bool env_enabled(const char* name);
std::string env_or_empty(const char* name);

// 启动阶段的致命错误：写日志 + 弹一个原生错误框，然后调用方收尾退出。
// GUI 是窗口程序（没有控制台），只写 SDL_Log 等于什么也没告诉用户 —— 崩溃在
// 启动路径上时，表现就是"双击没反应"。
void report_fatal(const std::string& message);

// 把 stdout / stderr / stdin 接回**父进程的控制台**（Windows）。
//
// 两个 GUI 按窗口子系统链接（CMake 的 WIN32_EXECUTABLE），自己没有控制台：
//   · 双击 / 从开始菜单启动 —— 没有父控制台，AttachConsole 失败，这里什么都不做，
//     于是也不会出现以前那个挂在后面的黑框；
//   · 从 cmd / PowerShell 启动 —— 接上父控制台，`--help`、日志、报错在终端里照样
//     看得见（不然窗口程序把这些打到 stdout 就等于扔进黑洞）。
// 非 Windows 平台无需处理（终端启动本来就有 stdout），函数是空实现。
// **必须**在 init_logger / 参数解析之前调用。
void attach_parent_console();

// 加载 UI 主字体（中文优先）与等宽字体（日志区）。
// 等宽字体不含中文字形：会把 UI 字体以 MergeMode 合并进去，否则日志里的中文是 '?'。
// 全部候选都缺失时装 ImGui 内置字体（窗口能开，中文变 '?'）——绝不留下空图集。
// AQUA_GUI_FONT=<路径> 覆盖 UI 字体；=none 跳过全部候选（验证降级路径）。
Fonts load_fonts(float ui_size, float mono_size);

struct WindowOptions {
    const char* title = "Aqua";
    int width = 620;       // 逻辑点
    int height = 820;      // 逻辑点
    int min_width = 600;   // 逻辑点
    int min_height = 480;
};

struct AppWindow {
    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    float content_scale = 1.0f; // DPI 内容缩放（1.0 = 96dpi）
};

// SDL_Init(VIDEO) 之后的建窗。内部处理：DPI 缩放、屏幕可用区收紧、最小尺寸、
// 高像素密度失败时回退、居中显示。失败返回 false（调用方负责 SDL_Quit）。
bool create_app_window(AppWindow& out, const WindowOptions& options);

// CreateContext + io 配置 + SDL3/SDLRenderer3 后端。失败时已自行回滚。
bool init_imgui(AppWindow& w);

void shutdown_imgui(AppWindow& w);
void destroy_app_window(AppWindow& w);

// 把 framebuffer 读回并写成 <dir>/aqua_gui.raw（RGBA 行主序）+ <dir>/aqua_gui.meta（宽 高）。
// 转 PNG 用 tools/raw2png.py（纯 zlib，无第三方依赖）。
void dump_framebuffer(SDL_Renderer* renderer, const std::string& dir);

} // namespace aqua::gui

#endif // AQUA_APP_GUI_APP_WINDOW_H
