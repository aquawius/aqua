#include "app_style.h"

#include <imgui.h>

namespace aqua::gui {
namespace {

// ImVec2 的数学运算符默认不启用（需 IMGUI_DEFINE_MATH_OPERATORS），
// 这里显式按分量缩放，不依赖该开关。
ImVec2 scaled(float x, float y, float s)
{
    return ImVec2(x * s, y * s);
}

} // namespace

void apply_theme(Theme theme, float size_scale)
{
    if (!(size_scale > 0.0f)) {
        size_scale = 1.0f;
    }

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

    // 统一重放间距度量（含 DPI 缩放）：既保证三个主题手感一致，也保证
    // 运行期切换主题后度量仍是缩放后的值（StyleColors* 不会碰它们，
    // 但显式重放可以防御将来 ImGui 默认值变化）。
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowPadding     = scaled(14.0f, 12.0f, size_scale);
    style.FramePadding      = scaled(8.0f, 5.0f, size_scale);
    style.ItemSpacing       = scaled(10.0f, 7.0f, size_scale);
    style.ItemInnerSpacing  = scaled(6.0f, 5.0f, size_scale);
    style.CellPadding       = scaled(6.0f, 4.0f, size_scale);
    style.IndentSpacing     = 18.0f * size_scale;
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

} // namespace aqua::gui
