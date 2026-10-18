#include "ui_panels.h"

#include "app_style.h"
#include "jb_presets.h"
#include "log_buffer.h"
#include "ui_layout.h"
#include "ui_state.h"

#include <aqua_app/cli/cli_version.h>

#include <imgui.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>

namespace aqua::gui {
namespace {

// 设置框的标题（OpenPopup / BeginPopupModal 必须同名）。
constexpr const char* kSettingsTitle = "设置";

// ---- 设置 → 外观 ----
// 主题 / 密度 / 圆角 / 字体 / 运行日志。全部即时生效（ImGui 1.92 惰性烘焙）。
// 连接行为（自动重连）不在这一页——那是"设置 → 连接"的事。
void draw_appearance_tab(UiState& s)
{
    ImGui::SeparatorText("主题");
    const Theme themes[] = { Theme::Dark, Theme::Light, Theme::Classic };
    for (int i = 0; i < 3; ++i) {
        if (i > 0) {
            ImGui::SameLine(0.0f, 24.0f);
        }
        if (ImGui::RadioButton(theme_name(themes[i]), s.theme == themes[i])) {
            s.theme = themes[i];
            apply_theme(s.theme, s.dpi_scale, s.density, s.rounding);
        }
    }

    ImGui::SeparatorText("界面");
    // 密度与圆角：即时生效、不持久化。密度只动留白，圆角动所有控件的角。
    const Density densities[] = { Density::Compact, Density::Normal, Density::Roomy };
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("密度");
    ImGui::SameLine(0.0f, 20.0f);
    for (int i = 0; i < 3; ++i) {
        if (i > 0) {
            ImGui::SameLine(0.0f, 18.0f);
        }
        if (ImGui::RadioButton(density_name(densities[i]), s.density == densities[i])) {
            s.density = densities[i];
            apply_theme(s.theme, s.dpi_scale, s.density, s.rounding);
        }
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("密度只影响留白（内边距 / 行距），不改变字号。");
    }

    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("圆角");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-FLT_MIN);
    int rounding = static_cast<int>(std::lround(s.rounding));
    if (ImGui::SliderInt("##rounding", &rounding, 0, 10, rounding == 0 ? "方角" : "%d px")) {
        s.rounding = static_cast<float>(rounding);
        apply_theme(s.theme, s.dpi_scale, s.density, s.rounding);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("0 = 方角（默认）；调大后输入框 / 卡片 / 弹窗一起变圆。");
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

    // 日志字号按用户 2026-10-18 要求从日志工具栏移到这里：日志是长期盯的东西，
    // 调好了不该在主界面常年占一个控件的位置。
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("日志字号");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(160.0f);
    ImGui::SliderInt("##log_font_size", &s.log_font_size, 10, 20, "%d px");
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("只作用于运行日志的输出框，与上面的 UI 字号相互独立。");
    }

    ImGui::SeparatorText("其他");
    if (ImGui::Button("恢复默认", ImVec2(120.0f, 0.0f))) {
        s.theme = Theme::Dark;
        s.density = Density::Normal;
        s.rounding = 0.0f;
        s.font_size = 16;
        s.log_min_level = kLogInfo;
        s.log_autoscroll = true;
        s.log_font_size = 12;
        apply_theme(s.theme, s.dpi_scale, s.density, s.rounding);
    }
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("设置不持久化：下次启动取默认值。");
}

// ---- 设置 → 连接 ----
// 只放"连接行为"：断线自动重连与重连间隔。地址 / 设备 / 网络预设仍在主界面
// 连接卡片上（那才是每次连接要确认的东西），这里不重复。
// 统计页只读 UiState，但设置框的 tab 表统一用 UiState& 签名，这里包一层。
void draw_stats_tab_editable(UiState& s)
{
    draw_stats_tab(s);
}

void draw_connection_tab(UiState& s)
{
    ImGui::SeparatorText("断线重连");
    ImGui::Checkbox("断线自动重连", &s.auto_reconnect);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("会话异常终止（网络中断 / 播放故障）后自动重建连接。\n"
                          "连接根本没建立起来（服务器不可达）时不重试。");
    }

    ImGui::BeginDisabled(!s.auto_reconnect);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("重连间隔");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(200.0f);
    ImGui::SliderInt("##reconnect_delay", &s.reconnect_delay_s, 1, 10, "%d 秒");
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("断线后等这么久再重建连接（与 Android App 同一策略）。");
    }
    ImGui::EndDisabled();

    ImGui::TextDisabled("改动于下次连接生效；连接中不受影响。");

    ImGui::SeparatorText("当前连接参数");
    // 只读摘要：让用户在这一页能看到"我将用什么去连"，不用回主界面确认。
    // 键列定宽（值从同一个 x 起），与统计页的两列表同一套读法。
    const ImGuiStyle& st = ImGui::GetStyle();
    const float key_w = ImGui::CalcTextSize("网络预设").x + st.ItemInnerSpacing.x * 2.0f;
    char endpoint[96];
    std::snprintf(endpoint, sizeof(endpoint), "%s:%s", s.rpc_ip, s.rpc_port);
    const char* const keys[2] = { "服务器", "网络预设" };
    const char* const values[2] = {
        endpoint,
        s.custom_config ? "自定义配置" : jb_preset_label(s.preset_index),
    };
    for (int i = 0; i < 2; ++i) {
        ImGui::TextDisabled("%s", keys[i]);
        ImGui::SameLine(key_w);
        ImGui::TextUnformatted(values[i]);
    }
}

// ---- 设置 → 高级 ----
// 抖动缓冲的 6 个参数：勾「自定义配置」后逐项可调（与 Android 的 AdvancedScreen
// 同一语义：滑条拖到 0 = 取 core 默认）；不勾时跟随主界面的网络预设档位，这里
// 只读展示档位解析出的值。范围 / 单位 / 默认值一律取自 jb_param_specs()——
// 与主界面预设滑条的 tooltip 同一数据源，不会两处讲两套口径。
void draw_advanced_tab(UiState& s)
{
    const JbParamSpec (&specs)[kJbParamCount] = jb_param_specs();
    const ImGuiStyle& st = ImGui::GetStyle();

    ImGui::SeparatorText("抖动缓冲高级参数");

    // JB 是连接属性：建连那一刻固定，连接中改了不生效——与主界面连接卡片
    // 同一口径（连接中 / 已连接锁住；演示模式不锁，那里没有真实会话）。
    const bool locked = !s.demo
        && (s.phase == UiPhase::Connecting || s.phase == UiPhase::Connected);
    if (locked) {
        ImGui::BeginDisabled();
    }
    bool custom = s.custom_config;
    if (ImGui::Checkbox("自定义配置", &custom)) {
        set_custom_config(s, custom); // 开启那刻把当前档位的值抄进 jb_custom 当起点
    }
    if (locked) {
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("连接中：抖动缓冲参数已在建连时固定，断开后才能改。");
        }
    }

    // 重置：6 项抄回当前档位的值（试坏几项后一键回到档位，不必重开对话框）。
    const float reset_w = ImGui::CalcTextSize("重置为档位值").x + st.FramePadding.x * 2.0f;
    same_line_right(reset_w);
    if (!s.custom_config) {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button("重置为档位值")) {
        s.jb_custom = jb_resolve(s.preset_index);
    }
    if (!s.custom_config) {
        ImGui::EndDisabled();
    }

    // 滑条要显示的当前值：自定义 = jb_custom 的原始值（0 = core 默认）；
    // 跟随预设 = 档位解析值（非 0），explicit_set 标出哪些是档位没设的。
    JbParamRow rows[kJbParamCount];
    if (s.custom_config) {
        for (int i = 0; i < kJbParamCount; ++i) {
            const double raw = jb_param_get(s.jb_custom, i);
            rows[i] = JbParamRow { specs[i].name, specs[i].unit, specs[i].desc,
                raw, raw != 0.0 };
        }
        ImGui::TextDisabled("逐项微调中：滑条拖到 0 = 取 core 默认 · 改动于下次连接生效。");
    } else {
        jb_param_rows(s.preset_index, rows);
        ImGui::TextDisabled("跟随主界面网络预设「%s」：以下为只读展示，勾选「自定义配置」后可逐项微调。",
            jb_preset_label(s.preset_index));
    }
    ImGui::Dummy(ImVec2(0.0f, 4.0f));

    const bool editable = s.custom_config && !locked;

    // 名称列定宽 = 最长参数名实测（字号调大时跟着变，不会溢出压到滑条上）。
    // 不用 TableSetupColumn(..., WidthFixed, 0.0f) 的"首帧按内容自适应"：那条路
    // 依赖 imgui 在后续帧量内容宽度，头几帧会被压成一两个字宽（截图实测整列只剩
    // 「缓」「最」「断」几个字 + 滑条塌成一条缝）。直接给定值，第一帧就是对的。
    float name_w = 0.0f;
    for (const JbParamSpec& spec : specs) {
        name_w = std::max(name_w, ImGui::CalcTextSize(spec.name).x);
    }
    name_w += st.CellPadding.x;

    // **能不能拖**完全由 editable 决定：没勾「自定义配置」时整张表禁用（只读展示
    // 档位解析值）。之前只把改动丢掉、滑条照样能拖，拖了没反应，等于骗用户。
    if (!editable) {
        ImGui::BeginDisabled();
    }
    if (ImGui::BeginTable("##jb_params", 2,
            ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_NoPadOuterX)) {
        ImGui::TableSetupColumn("param", ImGuiTableColumnFlags_WidthFixed, name_w);
        ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        for (int i = 0; i < kJbParamCount; ++i) {
            const JbParamSpec& spec = specs[i];
            const JbParamRow& row = rows[i];

            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(spec.name);
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                ImGui::SetTooltip("%s\n\n0 = core 默认（%g %s）",
                    spec.desc, spec.default_value, spec.unit);
            }

            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-FLT_MIN);
            // 值画在滑条框内（1.92 的滑条把标签画在框右侧，所以标签用独立
            // Text 放左列、滑条本体用空标签，见连接卡片的同一套规避手法）。
            // "档位没设"的项在值前加「默认」二字：读表的人不用去翻 tooltip
            // 就知道这个数是 core 给的，不是档位定的。
            char fmt[40];
            if (!row.explicit_set) {
                std::snprintf(fmt, sizeof(fmt), spec.integer ? "默认 %%d %s" : "默认 %%g %s",
                    spec.unit);
            } else {
                std::snprintf(fmt, sizeof(fmt), spec.integer ? "%%d %s" : "%%g %s", spec.unit);
            }
            ImGui::PushID(i);
            if (spec.integer) {
                int v = static_cast<int>(std::llround(row.value));
                if (ImGui::SliderInt("##jb", &v, static_cast<int>(spec.min),
                        static_cast<int>(spec.max), fmt)) {
                    if (editable) {
                        jb_param_set(s.jb_custom, i, static_cast<double>(v));
                    }
                }
            } else {
                double v = row.value;
                if (ImGui::SliderScalar("##jb", ImGuiDataType_Double, &v,
                        &spec.min, &spec.max, fmt)) {
                    if (editable) {
                        jb_param_set(s.jb_custom, i, v);
                    }
                }
            }
            ImGui::PopID();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                ImGui::SetTooltip("%s\n\n范围 %g – %g %s%s",
                    spec.desc, spec.min, spec.max, spec.unit,
                    editable ? "" : "\n\n先勾选上面的「自定义配置」才能拖动");
            }
        }
        ImGui::EndTable();
    }
    if (!editable) {
        ImGui::EndDisabled();
    }

    if (!editable && s.custom_config) {
        ImGui::TextDisabled("会话进行中：以上参数已在建连时固定，断开后可改。");
    }
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
    // 固定尺寸 + 可拖动改大小（不再 AlwaysAutoResize）：统计 tab 的内容比屏幕
    // 还高时会被裁掉一截（用户 2026-10-18："设置→统计 显示不全"）。改成定高 +
    // 内容区可滚动后，超长的内容走滚动条而不是溢出视口；用户也能拖大。
    // 首帧尺寸必须给：hidden 帧窗口是初始窄宽，靠 AutoResize 反推会在下一帧
    // 按窄宽换行把高度炸开、底边溢出视口（曾致"东西显示在外面"）。
    const ImVec2 work = ImGui::GetMainViewport()->WorkSize;
    const float max_h = std::max(320.0f, work.y - 40.0f);
    // 默认 640：统计页现在是"音频格式 + 实时指标 + 会话 + 网络 + 传输 + 心跳 +
    // 抖动缓冲"的全量明细，500 高只装得下两节。不够就滚动 / 拖动改大。
    ImGui::SetNextWindowSize(ImVec2(620.0f, std::min(640.0f, max_h)), ImGuiCond_Appearing);
    ImGui::SetNextWindowSizeConstraints(ImVec2(520.0f, 320.0f), ImVec2(920.0f, max_h));
    if (ImGui::BeginPopupModal(kSettingsTitle, &s.show_settings, ImGuiWindowFlags_NoCollapse)) {
        const bool want_appearance = (s.settings_tab == kSettingsAppearance);
        const bool want_connection = (s.settings_tab == kSettingsConnection);
        const bool want_stats = (s.settings_tab == kSettingsStats);
        const bool want_advanced = (s.settings_tab == kSettingsAdvanced);

        // 四个 tab 的骨架一样：请求位命中就 SetSelected，画完把请求清回 -1，
        // 内容区 0×0 child 自行滚动。
        struct TabSpec {
            const char* label;
            const char* child_id;
            bool wanted;
            void (*draw)(UiState&);
        };
        const TabSpec tabs[] = {
            { "外观", "##tab_appearance", want_appearance, &draw_appearance_tab },
            { "连接", "##tab_connection", want_connection, &draw_connection_tab },
            { "统计", "##tab_stats", want_stats, &draw_stats_tab_editable },
            { "高级", "##tab_advanced", want_advanced, &draw_advanced_tab },
        };
        if (ImGui::BeginTabBar("##settings_tabs")) {
            for (const TabSpec& tab : tabs) {
                if (ImGui::BeginTabItem(tab.label, nullptr,
                        tab.wanted ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None)) {
                    if (tab.wanted) {
                        s.settings_tab = -1; // 请求已兑现
                    }
                    // 内容区填满 tab 以下的剩余高度：内容超高时自己出滚动条，
                    // 不会把窗口顶大（BeginChild 0×0 = 吃掉当前可用区域）。
                    if (ImGui::BeginChild(tab.child_id, ImVec2(0.0f, 0.0f))) {
                        tab.draw(s);
                    }
                    ImGui::EndChild();
                    ImGui::EndTabItem();
                }
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
    ImGui::SetNextWindowSize(ImVec2(460.0f, 0.0f), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("关于 Aqua Client", &s.show_about,
            ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize)) {
        // 正经开源项目的"关于"：名字 / 版本 / 一句话定位 / 用到的开源组件（可点）/
        // 仓库与 issue 入口。不写阶段说明，不塞括注。
        ImGui::Text("Aqua Client");
        ImGui::TextDisabled("v%s", AQUA_CLIENT_CLI_VERSION);
        ImGui::TextDisabled("局域网音频共享客户端");
        ImGui::Dummy(ImVec2(0.0f, 6.0f));

        ImGui::SeparatorText("开源组件");
        struct Credit {
            const char* name;
            const char* url;
        };
        const Credit credits[] = {
            { "Dear ImGui — MIT", "https://github.com/ocornut/imgui" },
            { "SDL3 — Zlib", "https://github.com/libsdl-org/SDL" },
            { "spdlog — MIT", "https://github.com/gabime/spdlog" },
            { "Asio — Boost Software License", "https://github.com/chriskohlhoff/asio" },
            { "gRPC / Protocol Buffers — Apache-2.0", "https://github.com/grpc/grpc" },
            { "vcpkg — MIT", "https://github.com/microsoft/vcpkg" },
        };
        for (const Credit& credit : credits) {
            ImGui::TextLinkOpenURL(credit.name, credit.url);
        }

        ImGui::Dummy(ImVec2(0.0f, 6.0f));
        ImGui::SeparatorText("项目");
        ImGui::TextLinkOpenURL("github.com/aquawius/aqua", "https://github.com/aquawius/aqua");
        ImGui::TextLinkOpenURL("问题反馈 · Issues", "https://github.com/aquawius/aqua/issues");

        ImGui::Dummy(ImVec2(0.0f, 6.0f));
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
