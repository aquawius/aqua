#include "ui_panels.h"

#include "app_style.h"
#include "app_settings.h"
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
        // tooltip 挂在**每一项**上：放在循环外时 IsItemHovered 只指最后一个
        // RadioButton，前两项悬停没有任何说明。
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("密度只影响留白（内边距 / 行距），不改变字号。");
        }
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
        ImGui::SetTooltip("默认 5 px；拖到 0 = 方角。调大后输入框 / tab / 弹窗一起变圆。");
    }

    // 刷新率：main.cpp 的帧率节流直接读这个值（即时生效）。
    // 右端放实测帧率——"设置到底有没有生效"一眼可见，不用去猜。
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("刷新率");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(220.0f);
    if (ImGui::BeginCombo("##frame_rate", kFrameRateModeNames[s.frame_rate_mode])) {
        for (int i = 0; i < kFrameRateModeCount; ++i) {
            const bool selected = (i == s.frame_rate_mode);
            if (ImGui::Selectable(kFrameRateModeNames[i], selected)) {
                s.frame_rate_mode = i;
            }
            if (selected) {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("界面重绘频率（CPU 占用的主要来源）。\n"
                          "自适应 = 有鼠标/键盘操作时 60 fps，静止 1.5 s 后降到 15 fps（默认）。\n"
                          "指标数据是 500 ms 一拍，调高帧率只会让画面更顺滑，不会更快。");
    }
    {
        char fps_text[32];
        std::snprintf(fps_text, sizeof(fps_text), "当前 %d fps",
            static_cast<int>(s.render_fps + 0.5f));
        same_line_right(ImGui::CalcTextSize(fps_text).x);
        ImGui::TextDisabled("%s", fps_text);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("最近 0.5 秒的实际渲染帧率（含静止降频）。");
        }
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
    ImGui::TextDisabled("12 – 24 px，改动即时生效。");

    ImGui::SeparatorText("运行日志");
    // GUI 不提供 Trace（kGuiMinLogLevel）；下标 = 级别 - 下限。
    static const char* const kLevelNames[] = { "Debug", "Info", "Warn", "Error" };
    if (s.log_min_level < kGuiMinLogLevel) {
        s.log_min_level = kGuiMinLogLevel;
    }
    if (s.log_min_level > kLogError) {
        s.log_min_level = kLogError;
    }
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("级别");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120.0f);
    if (ImGui::BeginCombo("##set_log_level", kLevelNames[s.log_min_level - kGuiMinLogLevel])) {
        for (int i = kGuiMinLogLevel; i <= kLogError; ++i) {
            const bool selected = (i == s.log_min_level);
            if (ImGui::Selectable(kLevelNames[i - kGuiMinLogLevel], selected)) {
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
    // 日志行数上限（用户 2026-10-19）：开逐包 trace 时 2000 行几百毫秒就冲掉。
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("日志行数");
    ImGui::SameLine();
    // 宽度与上面的「字体大小」保持一致（都是整行填充）：两个滑条相邻，宽度不同
    // 会显得像两种控件（用户 2026-10-19 反馈）。
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::SliderInt("##log_capacity", &s.log_capacity, 200, 10000, "%d 行");
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("运行日志最多保留多少行（环形缓冲，旧的自动挤出）。\n"
                          "日志量大时（例如 Debug 级别）建议 ≥ 5000，否则很快被冲掉。\n"
                          "Ctrl + 点击滑条可以直接输入数值。");
    }
    ImGui::TextDisabled("200 – 10000 行，改动立即生效。");

    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("日志字号");
    ImGui::SameLine();
    // 宽度与上面的「字体大小」保持一致（都是整行填充）：两个滑条相邻，宽度不同
    // 会显得像两种控件（用户 2026-10-19 反馈）。
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::SliderInt("##log_font_size", &s.log_font_size, 10, 20, "%d px");
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("只作用于运行日志的输出框，与上面的 UI 字号相互独立。");
    }

    ImGui::SeparatorText("其他");
    if (ImGui::Button("恢复默认", ImVec2(120.0f, 0.0f))) {
        s.theme = Theme::Dark;
        s.density = Density::Normal;
        s.rounding = 5.0f;
        s.font_size = 16;
        s.log_min_level = kLogInfo;
        s.log_autoscroll = true;
        s.log_font_size = 14;
        s.log_capacity = 2000;
        s.frame_rate_mode = 0;
        apply_theme(s.theme, s.dpi_scale, s.density, s.rounding);
        save_settings(s); // 重置也要落盘，否则下次启动又回到旧值
    }
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("改动即时生效；关闭本窗口时自动保存。");
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("保存在：\n%s", settings_path().c_str());
    }
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
    // 键列定宽 = 最长键实测（三行共用同一个 x，值列才成一条竖线）。
    const float key_w = std::max(ImGui::CalcTextSize("网络预设").x,
                                 ImGui::CalcTextSize("客户端名称").x)
        + st.ItemInnerSpacing.x * 2.0f;
    char endpoint[96];
    std::snprintf(endpoint, sizeof(endpoint), "%s:%s", s.rpc_ip, s.rpc_port);
    const char* const keys[3] = { "服务器", "客户端名称", "网络预设" };
    const char* const values[3] = {
        endpoint,
        s.client_name,
        s.custom_config ? "自定义预设参数" : jb_preset_label(s.preset_index),
    };
    for (int i = 0; i < 3; ++i) {
        ImGui::TextDisabled("%s", keys[i]);
        ImGui::SameLine(key_w);
        ImGui::TextUnformatted(values[i]);
    }
}

// ---- 设置 → 高级 ----
// 抖动缓冲的 6 个参数：勾「自定义预设参数」后逐项可调（与 Android 的 AdvancedScreen
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
        && (s.phase == UiPhase::Connecting || s.phase == UiPhase::Reconnecting
            || s.phase == UiPhase::Connected);
    if (locked) {
        ImGui::BeginDisabled();
    }
    bool custom = s.custom_config;
    if (ImGui::Checkbox("自定义预设参数", &custom)) {
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

    // ---- 播放与容错 ----
    // 与上面 6 个滑条并列的一组**连接属性开关**（CLI 早就有对应开关，GUI 之前只
    // 暴露了数值旋钮）：--jb-fixed-target（反向）/ --jb-no-conceal（反向）/
    // --jb-no-splice（反向）/ --jb-trace / --playback-buffer-frames。
    // 同一套锁定规则：建连时固定，连接中置灰。
    // 刻意**不**放 playback.low_latency：只有 AAudio 后端读它，桌面后端忽略。
    ImGui::SeparatorText("播放与容错");
    if (locked) {
        ImGui::BeginDisabled();
    }

    // 4 个开关 + 1 个数值：勾选状态本身即时写入 UiState，但只有下次建连才生效
    // （JB / 播放流都是连接属性），所以这里只画控件，不做任何即时动作。
    ImGui::Checkbox("自适应缓冲目标", &s.jb_adaptive_target);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("开 = TargetController 按到达抖动动态调 target（默认）。\n"
                          "关 = 固定 target = 0.60N / 起步 0.50N（CLI 的 --jb-fixed-target），\n"
                          "用来做\"自适应到底有没有用\"的 A/B 对照。");
    }

    ImGui::Checkbox("PCM 掩盖", &s.jb_concealment);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("开 = 缺帧时重复上一个有效包 + 短淡出，超过连续上限转静音（默认）。\n"
                          "关 = 缺帧直接静音（CLI 的 --jb-no-conceal，v1 行为）。");
    }

    ImGui::Checkbox("拼接淡入淡出", &s.jb_splice);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("开 = DROP 着陆 / FILL 重播 / 掩盖进出都做 1.33ms 交叉淡入淡出，\n"
                          "只改输出样本、不动时间轴与控制（默认）。\n"
                          "关 = 整包硬拼接（CLI 的 --jb-no-splice），可 A/B 听感差别。");
    }

    ImGui::Checkbox("逐包 trace 日志（开发者）", &s.jb_packet_trace);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("每收到一个音频包打一行 JBT（到达 / 观测 / 决策），默认关。\n"
                          "约 274 行/秒（48k/175 帧）≈ 1.5MB/分钟。\n"
                          "这些行是 trace 级；GUI 不提供 Trace，只有 CLI（--log-level trace）能看到。");
    }

    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("回放缓冲");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120.0f);
    ImGui::InputInt("##pb_frames", &s.playback_buffer_frames, 0, 0);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("回放设备 I/O 粒度（采样帧；0 = 设备允许的最小周期，默认）。\n"
                          "注意这是**音频层**缓冲，不是链路层每包帧数 F；调小它会同时\n"
                          "降低自适应 target 的几何地板，换来的是更紧的调度裕度。");
    }
    ImGui::SameLine();
    ImGui::TextDisabled("0 = 设备允许的最小周期");

    if (locked) {
        ImGui::EndDisabled();
    }
    ImGui::TextDisabled("以上都是连接属性：改动于下次连接生效。");
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
    // 默认 700：高级 tab 现在是"6 个 JB 滑条 + 播放与容错 6 项"两节，统计 tab 是
    // 九节全量明细，640 高装不下（最后一两项被折在滚动区外，截图实测）。700 仍留
    // 出菜单栏与状态栏的余量；屏幕更小时由 max_h 夹住，用户也能拖动改大小。
    // 宽度 620 → 560、高度 700 → 660（用户 2026-10-19："框可以稍小一点，在正中间
    // 显示"）：主窗口宽 620，设置框原来和它一样宽、完全盖住背后；收窄 60px 后四周
    // 能露出主窗口，模态也不显得逼仄。定位仍是视口居中（Appearing + pivot 0.5）。
    ImGui::SetNextWindowSize(ImVec2(560.0f, std::min(660.0f, max_h)), ImGuiCond_Appearing);
    ImGui::SetNextWindowSizeConstraints(ImVec2(480.0f, 320.0f), ImVec2(920.0f, max_h));
    if (ImGui::BeginPopupModal(kSettingsTitle, &s.show_settings, ImGuiWindowFlags_NoCollapse)) {
        // Esc 关窗：这是所有桌面应用的肌肉记忆，且不需要键盘导航子系统的支持
        // （之前删掉的是"承诺了却做不到"的 Ctrl 组合键，不包含 Esc）。
        // 正在文本框里打字时不抢 Esc —— 那一下应该先退出编辑。
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) && !ImGui::GetIO().WantTextInput) {
            s.show_settings = false;
            ImGui::CloseCurrentPopup();
        }
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
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            s.show_about = false;
            ImGui::CloseCurrentPopup();
        }
        // 正经开源项目的"关于"：名字 / 版本 / 一句话定位 / 用到的开源组件（可点）/
        // 仓库与 issue 入口。不写阶段说明，不塞括注。
        ImGui::Text("Aqua Client");
        ImGui::TextDisabled("v%s", AQUA_CLIENT_CLI_VERSION);
        ImGui::TextDisabled("局域网音频共享客户端");
        ImGui::Dummy(ImVec2(0.0f, 6.0f));

        ImGui::SeparatorText("开源组件");
        // 只列组件名：许可证散在各项目的仓库里，写在关于窗里既占地方又容易过期
        // （用户 2026-10-18：把后缀去掉）。两列等宽排（用户同轮要求）：6 项占
        // 3 行，纵向高度与单列时的一半相当，窗口不用为此变高。
        struct Credit {
            const char* name;
            const char* url;
        };
        const Credit credits[] = {
            { "Dear ImGui", "https://github.com/ocornut/imgui" },
            { "SDL3", "https://github.com/libsdl-org/SDL" },
            { "spdlog", "https://github.com/gabime/spdlog" },
            { "Asio", "https://github.com/chriskohlhoff/asio" },
            { "gRPC / Protocol Buffers", "https://github.com/grpc/grpc" },
            { "vcpkg", "https://github.com/microsoft/vcpkg" },
        };
        constexpr int kCreditCount = static_cast<int>(sizeof(credits) / sizeof(credits[0]));
        if (ImGui::BeginTable("##credits", 2, ImGuiTableFlags_SizingStretchSame)) {
            for (int i = 0; i < kCreditCount; i += 2) {
                ImGui::TableNextRow();
                for (int c = 0; c < 2; ++c) {
                    ImGui::TableNextColumn();
                    const int idx = i + c;
                    if (idx < kCreditCount) {
                        ImGui::TextLinkOpenURL(credits[idx].name, credits[idx].url);
                    }
                }
            }
            ImGui::EndTable();
        }

        ImGui::Dummy(ImVec2(0.0f, 6.0f));
        // 项目区只留仓库入口（用户 2026-10-18）：点它跳到 GitHub 项目主页，
        // issue / 文档从仓库里走，不在关于窗里再开一串链接。
        ImGui::SeparatorText("项目");
        ImGui::TextLinkOpenURL("GitHub", "https://github.com/aquawius/aqua");

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
