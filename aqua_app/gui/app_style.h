#ifndef AQUA_APP_GUI_APP_STYLE_H
#define AQUA_APP_GUI_APP_STYLE_H

// aqua_client_gui 的视觉主题。
//
// 刻意选择 Dear ImGui 原生格式：颜色全部取 ImGui::StyleColorsDark() /
// StyleColorsLight() / StyleColorsClassic()，圆角 / 描边取 ImGuiStyle 默认值
// （全 0 圆角）——即开即得的经典 ImGui 外观（与 imgui_demo 及常见 ImGui 工具
// 一致）。UI 代码同样不写死任何色值；将来若要定制皮肤，只改 apply_theme() 一处。
//
// 间距度量只做"放宽"：默认 FramePadding(4,3) / ItemSpacing(8,4) 对表单类布局
// 偏挤，16px 字体下需要更舒适的呼吸空间。三个主题共用同一组度量，切换主题
// 只换颜色、不换手感。

namespace aqua::gui {

// 可选外观主题（ImHex 式：设置窗口里实时切换，v1 不持久化）。
enum class Theme {
    Dark,    // ImGui 原生深色（默认）
    Light,   // ImGui 原生浅色
    Classic, // ImGui 1.x 经典配色
};

// 写入主题：颜色 + 间距度量。幂等，可反复调用。
//   theme      — 目标主题
//   size_scale — DPI 内容缩放（main.cpp 传入 SDL_GetDisplayContentScale 的
//                返回值）；度量按它等比放大，传 1.0 = 96dpi 不缩放。
// 须在 ImGui::CreateContext() 之后调用；运行期切换主题（设置窗口）也调它。
// 字体缩放（FontScaleDpi / FontSizeBase）由 main.cpp 负责，不在这里动。
void apply_theme(Theme theme, float size_scale = 1.0f);

// 主题显示名（设置窗口 / 关于窗口用）。
const char* theme_name(Theme theme);

} // namespace aqua::gui

#endif // AQUA_APP_GUI_APP_STYLE_H
