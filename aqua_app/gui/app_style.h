#ifndef AQUA_APP_GUI_APP_STYLE_H
#define AQUA_APP_GUI_APP_STYLE_H

// aqua_client_gui 的视觉主题。
//
// 颜色取 Dear ImGui 原生格式：StyleColorsDark() / StyleColorsLight() /
// StyleColorsClassic() 为底，只对浅色主题做少量对比度修补（见 app_style.cpp）。
// UI 代码不写死任何色值；要定制皮肤，只改 apply_theme() 一处。
//
// 间距度量只做"放宽"：默认 FramePadding(4,3) / ItemSpacing(8,4) 对表单类布局
// 偏挤，16px 字体下需要更舒适的呼吸空间。三个主题共用同一组度量，切换主题
// 只换颜色、不换手感；密度（紧凑 / 标准 / 宽松）与圆角是用户可调的两个维度。

#include <imgui.h>

namespace aqua::gui {

// 可选外观主题（设置窗口里实时切换，不持久化）。
enum class Theme {
    Dark,    // ImGui 原生深色（默认）
    Light,   // ImGui 原生浅色 + 对比度修补
    Classic, // ImGui 1.x 经典配色
};

// 界面密度：只缩放留白（WindowPadding / ItemSpacing / …），不动字号。
enum class Density {
    Compact, // 一屏塞更多
    Normal,  // 默认
    Roomy,   // 更松
};

// 写入主题：颜色 + 间距度量 + 圆角。幂等，可反复调用。
//   theme      — 目标主题
//   size_scale — DPI 内容缩放（main.cpp 传入 SDL_GetDisplayContentScale 的
//                返回值）；度量按它等比放大，传 1.0 = 96dpi 不缩放。
//   density    — 留白密度（默认标准）
//   rounding   — 圆角像素（默认 5 = ImGui 原生观感；0 = 方角）
// 须在 ImGui::CreateContext() 之后调用；运行期切换（设置 → 外观）也调它。
// 字体缩放（FontScaleDpi / FontSizeBase）由 main.cpp 负责，不在这里动。
void apply_theme(Theme theme, float size_scale = 1.0f, Density density = Density::Normal,
    float rounding = 5.0f);

// 主题 / 密度的显示名（设置窗口用）。
const char* theme_name(Theme theme);
const char* density_name(Density density);

// ---- 语义色（跟随当前主题）----
// 警告黄 / 错误红不能写死：同一个 (0.93,0.80,0.32) 在深色底上是黄，在浅色底上
// 几乎看不见（对比度 ~1.3:1）。这两项由 apply_theme 记住当前主题后给出，
// 日志级别色、水位健康色、警戒线、错误提示统一走它们。
ImVec4 semantic_warn();
ImVec4 semantic_error();

} // namespace aqua::gui

#endif // AQUA_APP_GUI_APP_STYLE_H
