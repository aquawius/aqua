#include "app_style.h"

#include <imgui.h>

#include <algorithm>

namespace aqua::gui {
namespace {

// ImVec2 的数学运算符默认不启用（需 IMGUI_DEFINE_MATH_OPERATORS），
// 这里显式按分量缩放，不依赖该开关。
ImVec2 scaled(float x, float y, float s)
{
    return ImVec2(x * s, y * s);
}

// 界面密度 → 间距倍率（只作用在"留白"上，不碰字号）。
float density_scale(Density density)
{
    switch (density) {
    case Density::Compact:  return 0.78f;
    case Density::Roomy:    return 1.22f;
    case Density::Normal:
    default:                return 1.0f;
    }
}

// 浅色主题的默认配色有两个实际可用性问题（其余两个主题没有）：
//   · FrameBg 是纯白，与窗口底（0.94 灰）几乎同色 —— 输入框 / 滑条的**轨道**看不出
//     边界，滑块左右上限整个消失（用户 2026-10-18 反馈）；
//   · Border 太淡，卡片 / 分节线在浅色下糊成一片。
// 这里只压暗这几项，不动整体观感。
void patch_light_contrast(ImGuiStyle& style)
{
    style.Colors[ImGuiCol_FrameBg]        = ImVec4(0.878f, 0.878f, 0.898f, 1.00f);
    style.Colors[ImGuiCol_FrameBgHovered] = ImVec4(0.820f, 0.835f, 0.870f, 1.00f);
    style.Colors[ImGuiCol_FrameBgActive]  = ImVec4(0.775f, 0.795f, 0.845f, 1.00f);
    style.Colors[ImGuiCol_Border]         = ImVec4(0.690f, 0.700f, 0.730f, 0.85f);
    style.Colors[ImGuiCol_BorderShadow]   = ImVec4(0.000f, 0.000f, 0.000f, 0.00f);
    // 滑条的抓取块：默认是浅蓝，压在灰轨道上分不清"值在哪"。
    style.Colors[ImGuiCol_SliderGrab]       = ImVec4(0.290f, 0.510f, 0.800f, 1.00f);
    style.Colors[ImGuiCol_SliderGrabActive] = ImVec4(0.220f, 0.435f, 0.730f, 1.00f);
    // 表头 / 斑马纹同理：纯白背景上需要一点底色差。
    style.Colors[ImGuiCol_TableHeaderBg] = ImVec4(0.870f, 0.875f, 0.895f, 1.00f);
    style.Colors[ImGuiCol_TableRowBgAlt] = ImVec4(1.000f, 1.000f, 1.000f, 0.35f);
    style.Colors[ImGuiCol_Separator]     = ImVec4(0.690f, 0.700f, 0.730f, 0.90f);
}

// 当前主题的记忆：语义色（警告 / 错误 / 正常）要靠它决定用亮版还是暗版。
// apply_theme 是唯一写入点；UI 线程独占（ImGui 本身也只在 UI 线程用）。
Theme g_theme = Theme::Dark;

} // namespace

void apply_theme(Theme theme, float size_scale, Density density, float rounding)
{
    if (!(size_scale > 0.0f)) {
        size_scale = 1.0f;
    }
    g_theme = theme;

    // StyleColors*() 只改颜色、不动度量；三个主题的布局手感因此天然一致。
    switch (theme) {
    case Theme::Light:
        ImGui::StyleColorsLight();
        break;
    case Theme::Classic:
        ImGui::StyleColorsClassic();
        break;
    case Theme::Dark:
    default:
        ImGui::StyleColorsDark();
        break;
    }

    ImGuiStyle& style = ImGui::GetStyle();

    // 弹窗背景必须**不透明**：ImGui 默认 PopupBg 的 alpha 是 0.94，模态背后只要是
    // 一张有内容的页面（服务页的参数卡片 / 表格），底下的文字就会隐隐透出来，读起来
    // 像两页叠在一起（截图里能看到"loopback""系统默认（跟随）"透过表格）。
    // 设置框 / 关于框都吃这个颜色，client 与 server 一起受益。
    style.Colors[ImGuiCol_PopupBg].w = 1.0f;

    // 统一重放间距度量（含 DPI 缩放 × 密度倍率）：既保证三个主题手感一致，也保证
    // 运行期切换主题后度量仍是缩放后的值（StyleColors* 不会碰它们，但显式重放可以
    // 防御将来 ImGui 默认值变化）。密度只放大留白，字号由 FontSizeBase 单独管。
    const float s = size_scale * density_scale(density);
    style.WindowPadding    = scaled(14.0f, 12.0f, s);
    style.FramePadding     = scaled(8.0f, 5.0f, s);
    style.ItemSpacing      = scaled(10.0f, 7.0f, s);
    style.ItemInnerSpacing = scaled(6.0f, 5.0f, s);
    style.CellPadding      = scaled(6.0f, 4.0f, s);
    style.IndentSpacing    = 18.0f * s;
    style.ScrollbarSize    = 14.0f * std::max(size_scale, 1.0f);
    style.GrabMinSize      = 10.0f * size_scale;

    // 圆角（设置 → 外观）：默认 5 px = ImGui 原生观感（tab / 输入框 / 卡片 / 弹窗
    // 一起圆）；拖到 0 = 方角。注：遥测面板两张自绘卡片的圆角另在 ui_telemetry.cpp
    // 的 begin_card 里按同一个值画——ImGui 的度量管不到 AddRect 手绘的矩形。
    const float r = (rounding > 0.0f) ? rounding : 0.0f;
    style.WindowRounding    = r;
    style.ChildRounding     = r;
    style.FrameRounding     = r;
    style.PopupRounding     = r;
    style.ScrollbarRounding = r;
    style.GrabRounding      = r;
    style.TabRounding       = r;

    if (theme == Theme::Light) {
        patch_light_contrast(style);
    }
}

const char* theme_name(Theme theme)
{
    switch (theme) {
    case Theme::Light:   return "浅色";
    case Theme::Classic: return "经典";
    case Theme::Dark:
    default:             return "深色";
    }
}

const char* density_name(Density density)
{
    switch (density) {
    case Density::Compact: return "紧凑";
    case Density::Roomy:   return "宽松";
    case Density::Normal:
    default:               return "标准";
    }
}

// 语义色：浅色主题下用深版，否则白底上的黄字 / 亮红字几乎看不见。
ImVec4 semantic_warn()
{
    return (g_theme == Theme::Light) ? ImVec4(0.62f, 0.44f, 0.02f, 1.0f)
                                     : ImVec4(0.93f, 0.80f, 0.32f, 1.0f);
}

ImVec4 semantic_error()
{
    return (g_theme == Theme::Light) ? ImVec4(0.72f, 0.12f, 0.10f, 1.0f)
                                     : ImVec4(0.94f, 0.42f, 0.38f, 1.0f);
}

} // namespace aqua::gui
