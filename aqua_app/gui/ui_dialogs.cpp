#include "ui_panels.h"

#include "app_style.h"
#include "jb_presets.h"
#include "log_buffer.h"
#include "ui_state.h"

#include <aqua_app/cli/cli_version.h>

#include <imgui.h>

#include <cfloat>

namespace aqua::gui {
namespace {

// 设置框的标题（OpenPopup / BeginPopupModal 必须同名）。
constexpr const char* kSettingsTitle = "设置";

// ---- 设置 → 外观 ----
// 主题 / 字体 / 运行日志 / 连接。全部即时生效（ImGui 1.92 惰性烘焙）。
void draw_appearance_tab(UiState& s)
{
    ImGui::SeparatorText("主题");
    if (ImGui::RadioButton("深色", s.theme == Theme::Dark)) {
        s.theme = Theme::Dark;
        apply_theme(s.theme, s.dpi_scale);
    }
    ImGui::SameLine(0.0f, 24.0f);
    if (ImGui::RadioButton("浅色", s.theme == Theme::Light)) {
        s.theme = Theme::Light;
        apply_theme(s.theme, s.dpi_scale);
    }
    ImGui::SameLine(0.0f, 24.0f);
    if (ImGui::RadioButton("经典", s.theme == Theme::Classic)) {
        s.theme = Theme::Classic;
        apply_theme(s.theme, s.dpi_scale);
    }

    ImGui::SeparatorText("字体");
    // 1.92 滑条标签画在框右侧：标签独立 Text 放左，滑条空标签填充，
    // 档位值由 format 画在框内（与网络预设滑条同一套规避手法）。
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("字体大小");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::SliderInt("##font_size", &s.font_size, 12, 24, "%d px")) {
        // 拖动即改；main 循环每帧写 style.FontSizeBase，惰性烘焙即时生效。
    }
    ImGui::TextDisabled("12 – 24 px，改动即时生效（ImGui 惰性烘焙，无需重启）。");

    ImGui::SeparatorText("运行日志");
    static const char* const kLevelNames[] = { "Debug", "Info", "Warn", "Error" };
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("级别");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120.0f);
    if (ImGui::BeginCombo("##set_log_level", kLevelNames[s.log_min_level])) {
        for (int i = 0; i < 4; ++i) {
            const bool selected = (i == s.log_min_level);
            if (ImGui::Selectable(kLevelNames[i], selected)) {
                s.log_min_level = i;
            }
            if (selected) {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine(0.0f, 24.0f);
    ImGui::Checkbox("自动滚动", &s.log_autoscroll);

    ImGui::SeparatorText("连接");
    // 断线自动重连按用户 2026-10-16 要求从主界面连接卡片移至此处（主界面只留
    // 固定使用此设备）。恢复默认不含它——重连是连接行为不是外观。
    ImGui::Checkbox("断线自动重连", &s.auto_reconnect);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("断线约 3 秒后自动重建连接（与 Android App 一致的策略）。\n当前为界面骨架阶段，会话接入后生效。");
    }

    ImGui::SeparatorText("其他");
    if (ImGui::Button("恢复默认", ImVec2(120.0f, 0.0f))) {
        s.theme = Theme::Dark;
        s.font_size = 16;
        s.log_min_level = kLogInfo;
        s.log_autoscroll = true;
        apply_theme(s.theme, s.dpi_scale);
    }
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("v1 不持久化：下次启动仍取默认值。");
}

// ---- 设置 → 高级 ----
// 当前网络预设解析出的完整 JB 参数（真实数据，随卡片滑条实时变化）。
// 每行标注来源：本档位显式设置 / core 默认——与滑条 tooltip 同一数据源
// （jb_param_rows），两处不会讲两套话。
void draw_advanced_tab(UiState& s)
{
    JbParamRow rows[kJbParamCount];
    jb_param_rows(s.preset_index, rows);

    ImGui::SeparatorText("网络预设高级参数");
    ImGui::TextDisabled("当前档位：%s（%s）· %s",
        jb_preset_label(s.preset_index), jb_preset_hint(s.preset_index),
        s.custom_config ? "自定义配置已启用" : "跟随预设档位");
    ImGui::Dummy(ImVec2(0.0f, 4.0f));

    if (ImGui::BeginTable("##jb_params", 4,
            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg
                | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("参数", ImGuiTableColumnFlags_WidthFixed, 96.0f);
        ImGui::TableSetupColumn("值", ImGuiTableColumnFlags_WidthFixed, 92.0f);
        ImGui::TableSetupColumn("来源", ImGuiTableColumnFlags_WidthFixed, 84.0f);
        ImGui::TableSetupColumn("说明", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        for (const JbParamRow& r : rows) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(r.name);
            ImGui::TableNextColumn();
            ImGui::Text("%g %s", r.value, r.unit);
            ImGui::TableNextColumn();
            if (r.explicit_set) {
                ImGui::TextUnformatted("本档位");
            } else {
                ImGui::TextDisabled("core 默认");
            }
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", r.desc);
        }
        ImGui::EndTable();
    }

    ImGui::Dummy(ImVec2(0.0f, 4.0f));
    ImGui::TextDisabled("自定义微调（握手间隔 / 客户端名 / UDP 端口覆盖）随会话层接入后开放；"
                        "JB 参数是连接属性，改动于下次连接生效。");
}

} // namespace

// ---- 设置窗口（模态弹出，多 tab：外观 / 统计 / 高级）----
// 模态 = BeginPopupModal：imgui 自动给背景压暗（ImGuiCol_ModalWindowDimBg，
// NewFrame 里按 GetTopMostPopupModal 调 DimBgRatio）并拦截其他窗口的输入——
// 关闭前点不了主窗口的任何控件。p_open 与标题栏 X 按钮联动；自绘关闭按钮
// 需额外 CloseCurrentPopup()（只置 p_open 不会关掉当前弹出）。
//
// tab 请求位：菜单项 / 卡片「高级参数…」按钮置 UiState::settings_tab 后打开
// 本框，对应 tab 一旦被真正选中就清回 -1。不能每帧无脑 SetSelected——用户在
// 框内手动切走的 tab 会被拽回来（第 9 轮踩过的坑）；模态挡住主窗口输入，所以
// 请求只可能在框关闭时置位，不存在"开着框时被顶掉"的交错。
void draw_settings_window(UiState& s)
{
    if (!s.show_settings) {
        return;
    }
    ImGui::OpenPopup(kSettingsTitle);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    // 首帧宽度必须给（AlwaysAutoResize 的 hidden 帧窗口还是初始窄宽，下一帧
    // 内容按窄宽换行会把高度炸开、底边溢出视口——曾致"东西显示在外面"）。
    ImGui::SetNextWindowSize(ImVec2(560.0f, 0.0f), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal(kSettingsTitle, &s.show_settings,
            ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize)) {
        const bool want_appearance = (s.settings_tab == kSettingsAppearance);
        const bool want_stats = (s.settings_tab == kSettingsStats);
        const bool want_advanced = (s.settings_tab == kSettingsAdvanced);

        if (ImGui::BeginTabBar("##settings_tabs")) {
            if (ImGui::BeginTabItem("外观", nullptr,
                    want_appearance ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None)) {
                if (want_appearance) {
                    s.settings_tab = -1; // 请求已兑现
                }
                draw_appearance_tab(s);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("统计", nullptr,
                    want_stats ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None)) {
                if (want_stats) {
                    s.settings_tab = -1;
                }
                draw_stats_tab(s);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("高级", nullptr,
                    want_advanced ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None)) {
                if (want_advanced) {
                    s.settings_tab = -1;
                }
                draw_advanced_tab(s);
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }

        // EndPopup 必须在 if 内：BeginPopupModal 返回 false 时（首帧测量帧 /
        // 关闭帧）内部已自行 EndPopup + ClosePopupToLevel（imgui.cpp:12724），
        // 此时当前窗口并不是弹窗，再调一次 EndPopup 必触发
        // "Calling EndPopup() in wrong window!" assert（曾致打开即崩）。
        ImGui::EndPopup();
    }
}

// ---- 关于窗口（模态弹出）----
void draw_about_window(UiState& s)
{
    if (!s.show_about) {
        return;
    }
    ImGui::OpenPopup("关于 Aqua Client");
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    // AlwaysAutoResize + PushTextWrapPos(0) 有个坑：hidden 帧窗口宽度还是初始窄宽，
    // 下一帧 TextWrapped 按这个窄宽换行 → 高度炸到接近视口高（实测 751/757），
    // 弹窗底边溢出视口（用户所见"东西显示在外面"）。给一个合理首帧宽度即可根治。
    ImGui::SetNextWindowSize(ImVec2(420.0f, 0.0f), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("关于 Aqua Client", &s.show_about,
            ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Aqua Client — 音频共享桌面客户端");
        ImGui::TextDisabled("版本 v%s（与 aqua_client_cli 同源）", AQUA_CLIENT_CLI_VERSION);
        ImGui::TextDisabled("界面 Dear ImGui %s（SDL3 platform + SDLRenderer3 后端）", IMGUI_VERSION);
        ImGui::Separator();
        ImGui::TextWrapped("当前阶段：界面骨架。连接 / 断开、播放设备热切换、实时诊断随会话层"
            "（ClientRuntime）接入后开放；网络预设滑条现在即可切换，改动于下次连接生效。");
        ImGui::Dummy(ImVec2(0.0f, 4.0f));
        if (ImGui::Button("关闭", ImVec2(-FLT_MIN, 0.0f))) {
            s.show_about = false;
            ImGui::CloseCurrentPopup(); // 只置 p_open 不会关当前弹出，必须显式关
        }
        // EndPopup 只在 BeginPopupModal 返回 true 时调用（false 时内部已配对，
        // 原因见 draw_settings_window 内的注释）。
        ImGui::EndPopup();
    }
}

} // namespace aqua::gui
