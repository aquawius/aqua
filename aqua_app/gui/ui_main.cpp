#include "ui_main.h"

#include <aqua_app/cli/cli_version.h>
#include <aqua/audio/devices/audio_device_manager.h>

#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <ctime>
#include <exception>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace aqua::gui {
namespace {

// 等宽字号 = UI 基准字号 × 0.875（16 → 14，与早期手工调校值一致）。
// 注意 PushFont 接收的是"未缩放"基准值：style.FontScaleDpi 会统一再乘，
// 所以这里直接用 UiState.font_size 推导，不掺 DPI。
constexpr float kMonoRatio = 0.875f;

float mono_size(const UiState& s)
{
    return static_cast<float>(s.font_size) * kMonoRatio;
}

// ---- 网络预设表 ----
// 数值与 Android 的 JbPreset 逐档对齐（容量 / 下限 / stall 峰值 / 衰减 / 欠载惩罚）。
// 字段为 0 = 不显式设置、取 core 默认（容量 30 / 下限 3 / 峰值 12 / 衰减 5 / 惩罚 1）。
// 与 Android 的一处有意分歧：Android 的 stallThresholdPackets 缺省传 0（= 关检测），
// 桌面 GUI 与 CLI 一致——不传即 core 默认 5.0（检测开启）。
// 会话接入时整表映射到 ClientRuntimeConfig 的 jb_* 字段；JB 参数是连接属性、
// 运行期不可改，UI 统一标注"改动于下次连接生效"。
struct JbPresetRow {
    const char* label;
    const char* short_hint;
    std::uint32_t capacity = 0;
    std::uint32_t min_target = 0;
    double stall_peak_cap = 0.0;
    double stall_decay = 0.0;
    double stall_threshold = 0.0;
    double underrun_penalty = 0.0;
};

constexpr JbPresetRow kJbPresets[] = {
    { "极低延迟", "容量 16 槽", 16 },
    { "LAN", "下限 5 槽", 0, 5, 8.0, 10.0 },
    { "Wi-Fi", "下限 7 槽", 0, 7, 10.0, 10.0 },
    { "Wi-Fi 稳定", "下限 9 槽", 0, 9, 12.0, 10.0 },
    { "Wi-Fi 拥挤", "下限 12 槽", 0, 12, 14.0, 10.0 },
    { "公网", "容量 60 槽", 60, 12, 16.0, 5.0 },
    { "50 ms", "下限 14 槽", 60, 14, 16.0, 5.0 },
    { "100 ms", "下限 27 槽", 60, 27, 16.0, 5.0 },
    { "弱网", "容量 80 槽", 80, 32, 16.0, 5.0, 0.0, 2.0 },
    { "200 ms", "下限 55 槽", 120, 55, 20.0, 5.0, 0.0, 2.0 },
    { "恶劣", "容量 160 槽", 160, 80, 24.0, 3.0, 0.0, 3.0 },
};
constexpr int kPresetCount = static_cast<int>(sizeof(kJbPresets) / sizeof(kJbPresets[0]));

// ---- 播放设备下拉的数据源 ----
// 设备 ID 跨会话不稳定（WASAPI endpoint id 每次运行可能变），列表只作当期选择、
// 不持久化；index 0 固定"系统默认（跟随）"。每 2s 刷新一次（设备枚举自带 COM
// 初始化、任意线程可调；这里就在 UI 线程做，频次很低）。
struct DeviceCache {
    std::unique_ptr<aqua::audio::AudioDeviceManager> manager;
    std::vector<std::string> names;
    bool platform_supported = true;
    double last_refresh = -1000.0;
};

void refresh_devices(DeviceCache& cache)
{
    const double now = ImGui::GetTime();
    if (now - cache.last_refresh < 2.0) {
        return;
    }
    cache.last_refresh = now;

    try {
        if (cache.manager == nullptr) {
            cache.manager = aqua::audio::create_device_manager();
        }
        cache.names.clear();
        cache.names.emplace_back("系统默认（跟随）");
        if (cache.manager == nullptr) {
            cache.platform_supported = false;
            cache.names.emplace_back("（当前平台没有音频后端）");
            return;
        }
        cache.platform_supported = true;
        for (const aqua::audio::AudioDevice& dev : cache.manager->enumerate(aqua::audio::AudioDeviceDirection::OUTPUT)) {
            cache.names.push_back(dev.name);
        }
    } catch (const std::exception& e) {
        cache.platform_supported = false;
        cache.names.clear();
        cache.names.emplace_back("系统默认（跟随）");
        cache.names.emplace_back(std::string("（设备枚举失败：") + e.what() + "）");
    }
}

// ---- 演示数据（AQUA_GUI_DEMO=1；不发起任何真实连接）----
void seed_demo_logs()
{
    LogBuffer& log = log_buffer();
    log.append(kLogInfo, "aqua_client_gui 启动（演示模式：拟真数据，未发起真实连接）");
    log.append(kLogInfo, "输出设备枚举完成，默认跟随系统");
    log.append(kLogInfo, "演示会话 #A3F1：48 kHz / 2ch / F32LE，175 帧每包");
    log.append(kLogDebug, "JitterBuffer: capacity=30 slots, adaptive target on, min_target=7");
    log.append(kLogWarn, "到达抖动 3.2 ms，缓冲水位处于 Warning 带（软校正）");
}

// 6 个指标的演示值：{ water_level, lead_ms, target_ms, jitter_ms, loss%, underrun_ratio }
void fill_demo_tiles(double (&v)[6])
{
    const double t = ImGui::GetTime();
    v[0] = 0.42 + 0.05 * std::sin(t * 0.7);
    v[1] = 26.0 + 2.0 * std::sin(t * 0.5);
    v[2] = 26.0 + 2.0 * std::sin(t * 0.5 + 0.4);
    v[3] = 3.2 + 0.6 * std::sin(t * 1.1);
    v[4] = 0.01;
    v[5] = 0.0;
}

std::string demo_duration(double connected_at)
{
    int total = static_cast<int>(ImGui::GetTime() - connected_at);
    if (total < 0) {
        total = 0;
    }
    const int h = total / 3600;
    const int m = (total / 60) % 60;
    const int s = total % 60;
    char buf[24];
    if (h > 0) {
        std::snprintf(buf, sizeof(buf), "%d:%02d:%02d", h, m, s);
    } else {
        std::snprintf(buf, sizeof(buf), "%02d:%02d", m, s);
    }
    return buf;
}

// ---- 小工具 ----
std::string now_hms()
{
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[16];
    std::strftime(buf, sizeof(buf), "%H:%M:%S", &tm);
    return buf;
}

// 把后续控件排到"当前行右侧、距内容右缘 width 像素"处（右对齐控件组用）。
// 关键坑：SetCursorPosX 只改 X——ItemSize 末尾会清掉 IsSameLine
// （imgui.cpp:11622），下一个 item 仍另起一行：状态栏右侧文本曾因此掉到
// 窗口底缘之外（用户报"最下面显示超出范围，而且不对齐"）。必须用
// SameLine(offset) 回到同一行：offset 相对"窗口左缘 - 滚动"
// （imgui.cpp:11646），而 GetWindowContentRegionMax() 返回的正是相对
// window->Pos 的坐标（其源码：ContentRegionRect.Max - window->Pos），两者
// 基准一致；Y 由 SameLine 内部置回 CursorPosPrevLine.y（imgui.cpp:11647）。
// spacing 传 0 不再附加间距。
void same_line_right(float width)
{
    ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - width, 0.0f);
}

// 把后续控件排到"下一行右侧、距内容右缘 width 像素"处（跟在 full-width 控件
// 之后的整行右对齐用）。与 same_line_right 的分工：
//   same_line_right   —— 和当前行控件并排（SameLine 把 Y 拽回上一行）；
//   set_cursor_right_x —— 就在下一行，只改 X。
// 关键坑（2026-10-16 用户截图实测：勾选框压住播放设备下拉框、压住网络预设
// 滑条）：跟在 full-width 控件（下拉框 / 滑条）后面用 same_line_right，会把
// 本该在下一行的控件拽回上一行、与那个控件重叠。此时上一个 item 提交后光标
// 已经停在下一行（ItemSize 的推进），用 SetCursorPosX 只调 X 即可——它不置
// IsSameLine，而 IsSameLine 刚被 ItemSize 清掉（imgui.cpp:11622），下一个
// item 自然落在当前行。
void set_cursor_right_x(float width)
{
    ImGui::SetCursorPosX(ImGui::GetWindowContentRegionMax().x - width);
}

const char* level_tag(int level)
{
    switch (level) {
    case kLogDebug: return "DEBUG";
    case kLogWarn:  return "WARN";
    case kLogError: return "ERROR";
    default:        return "INFO";
    }
}

const char* phase_text(UiPhase phase)
{
    switch (phase) {
    case UiPhase::Connecting:    return "连接中…";
    case UiPhase::Connected:     return "已连接";
    case UiPhase::Disconnecting: return "断开中…";
    case UiPhase::Idle:
    default:                     return "未连接";
    }
}

// ---- 指标条 ----
// 原生表格：6 列等宽，第一行标签、第二行数值（等宽字体）。
// 字段口径与 ClientDiagnosticsSnapshot 对齐（会话接入后从这里取数）：
//   water_level / lead_ms / target_ms / estimator_jitter_ms / 丢包率 / underrun_ratio
void draw_metrics_strip(const UiState& s)
{
    static constexpr const char* kLabels[6] = { "缓冲水位", "领先", "目标", "网络抖动", "丢包", "欠载" };
    char values[6][24];
    const bool live = (s.phase == UiPhase::Connected);
    if (live && s.demo) {
        double v[6];
        fill_demo_tiles(v);
        std::snprintf(values[0], sizeof(values[0]), "%.0f %%", v[0] * 100.0);
        std::snprintf(values[1], sizeof(values[1]), "%.1f ms", v[1]);
        std::snprintf(values[2], sizeof(values[2]), "%.1f ms", v[2]);
        std::snprintf(values[3], sizeof(values[3]), "%.1f ms", v[3]);
        std::snprintf(values[4], sizeof(values[4]), "%.2f %%", v[4]);
        std::snprintf(values[5], sizeof(values[5]), "%.2f %%", v[5] * 100.0);
    } else {
        for (auto& row : values) {
            std::snprintf(row, sizeof(row), "%s", "-");
        }
    }

    if (ImGui::BeginTable("##metrics", 6,
            ImGuiTableFlags_Borders | ImGuiTableFlags_SizingStretchSame)) {
        for (int i = 0; i < 6; ++i) {
            ImGui::TableNextColumn();
            ImGui::Text("%s", kLabels[i]);
        }
        ImGui::TableNextRow();
        for (int i = 0; i < 6; ++i) {
            ImGui::TableNextColumn();
            ImGui::PushFont(s.font_mono, mono_size(s));
            ImGui::TextUnformatted(values[i]);
            ImGui::PopFont();
        }
        ImGui::EndTable();
    }
}

// ---- 菜单栏（ImHex 式：viewport 级主菜单，功能与卡片控件同源）----
void draw_menu_bar(UiState& s)
{
    if (!ImGui::BeginMainMenuBar()) {
        return;
    }
    if (ImGui::BeginMenu("文件")) {
        if (ImGui::MenuItem("连接", "Ctrl+K", false, s.phase == UiPhase::Idle)) {
            toggle_connection(s);
        }
        if (ImGui::MenuItem("断开", nullptr, false, s.phase == UiPhase::Connected)) {
            toggle_connection(s);
        }
        ImGui::Separator();
        if (ImGui::MenuItem("退出")) {
            s.quit_requested = true;
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("视图")) {
        // 上下分屏后连接 / 运行日志都常驻可见，原来的"连接页 / 运行日志"切页
        // 菜单项与 Ctrl+L 一并取消（没有"当前页"可切了）。
        if (ImGui::MenuItem("诊断面板", nullptr, s.show_diagnostics)) {
            s.show_diagnostics = !s.show_diagnostics;
        }
        if (ImGui::MenuItem("高级设置", nullptr, s.show_advanced)) {
            s.show_advanced = !s.show_advanced;
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("设置")) {
        if (ImGui::MenuItem("外观设置…", "Ctrl+P")) {
            s.show_settings = true;
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("帮助")) {
        if (ImGui::MenuItem("关于 Aqua Client")) {
            s.show_about = true;
        }
        ImGui::EndMenu();
    }
    ImGui::EndMainMenuBar();
}

// ---- 连接卡片 ----
void draw_connection_card(UiState& s, DeviceCache& devices)
{
    // 设备列表可能因插拔变短，夹住下标。
    if (s.device_index < 0 || s.device_index >= static_cast<int>(devices.names.size())) {
        s.device_index = 0;
    }

    // 边框卡片高度自适应内容（不满高时下面是干净的窗口底色，原生观感）。
    // 判返回值：被父区域裁剪完时 BeginChild 返回 false，继续提交会配对错乱。
    if (!ImGui::BeginChild("##connect_card", ImVec2(0.0f, 0.0f),
            ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY)) {
        return;
    }

    // ---- 服务器地址 + gRPC 端口 + UDP 端口（标签一行、输入框一行）----
    // 两行三列：第一行三个标签、第二行三个输入框；列宽 3:1:1.3 等距拉伸，
    // IP 列最宽、端口列收窄。
    // 为什么拆两行而不是"每格 标签+输入框"的单行：imgui 表格会把单元格里
    // 第一个 framed 控件（InputText，ItemSize 的 text_baseline = FramePadding.y）
    // 经 TableEndCell 传播成 RowTextBaseline（imgui_tables.cpp），后续单元格
    // 的 Text 标签（ItemSize 基线传 0，imgui_widgets.cpp TextEx）行高被垫高
    // 一个 FramePadding.y，输入框随之低 5px——三列输入框因此不在一条水平线
    // 上（2026-10-16 截图实测：第一列比其余列高 4~5px，用户报"水平不对齐"）。
    // 拆行后每格都是行首 item，基线传播只影响行高、不影响 item 的绘制位置，
    // 三列自然对齐（指标条表格本就是这结构，所以从不错位）。
    if (ImGui::BeginTable("##addr", 3, ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("ip", ImGuiTableColumnFlags_WidthStretch, 3.0f);
        ImGui::TableSetupColumn("rpc", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("udp", ImGuiTableColumnFlags_WidthStretch, 1.3f);

        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::Text("服务器地址");
        ImGui::TableNextColumn();
        ImGui::Text("gRPC 端口");
        ImGui::TableNextColumn();
        ImGui::Text("UDP 端口");

        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputTextWithHint("##rpc_ip", "192.168.1.10", s.rpc_ip, sizeof(s.rpc_ip));
        ImGui::TableNextColumn();
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputTextWithHint("##rpc_port", "50051", s.rpc_port, sizeof(s.rpc_port),
            ImGuiInputTextFlags_CharsDecimal);
        ImGui::TableNextColumn();
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputTextWithHint("##udp_port", "留空 = 跟随 Server", s.udp_port, sizeof(s.udp_port),
            ImGuiInputTextFlags_CharsDecimal);

        ImGui::EndTable();
    }

    // ---- 播放设备（index 0 = 系统默认）----
    ImGui::Text("播放设备");
    ImGui::SetNextItemWidth(-FLT_MIN);
    const char* preview = devices.names[static_cast<std::size_t>(s.device_index)].c_str();
    if (ImGui::BeginCombo("##device", preview)) {
        for (int i = 0; i < static_cast<int>(devices.names.size()); ++i) {
            const bool selected = (i == s.device_index);
            if (ImGui::Selectable(devices.names[static_cast<std::size_t>(i)].c_str(), selected)) {
                s.device_index = i;
            }
            if (selected) {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }
    if (!devices.platform_supported && ImGui::IsItemHovered()) {
        ImGui::SetTooltip("当前平台没有音频后端（Linux / macOS 播放待实现）。\nWindows 上这里会列出全部输出设备。");
    }

    // ---- 勾选框：设备钉住（单独一行、靠右）----
    // 用户 2026-10-16：固定使用此设备与断线自动重连不再并排；断线自动重连移至
    // 设置窗口（draw_settings_window 的「连接」段）；固定使用此设备所在行不变、
    // 改为右对齐。勾选框总宽 = GetFrameHeight()（方框边长）+ ItemInnerSpacing.x
    // + 文本宽（imgui_widgets.cpp Checkbox 的 total_bb 公式），据此右对齐。
    // 必须用 set_cursor_right_x 而非 same_line_right：上面是全宽下拉框，用
    // SameLine 会把勾选框拽回下拉框那一行（重叠）。
    const ImGuiStyle& st = ImGui::GetStyle();
    const float pin_cb_w = ImGui::GetFrameHeight() + st.ItemInnerSpacing.x
        + ImGui::CalcTextSize("固定使用此设备").x;
    set_cursor_right_x(pin_cb_w);
    ImGui::Checkbox("固定使用此设备", &s.pin_device);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("勾选 = 钉住下拉所选设备（设备消失时按回退链处理）；\n不勾 = 跟随系统默认输出设备。");
    }

    // ---- 网络预设滑条（11 档）：标签独立一行，滑条下一行 ----
    // 用户设计变更（2026-10-16）：标签不再与滑条挤在同一行。ImGui 1.92 的
    // 滑条把标签画在滑条框的右侧，若给滑条设填充宽度（-FLT_MIN）标签会被
    // 挤出内容区右缘遭裁切，因此标签用独立 Text、滑条用空标签填充剩余宽度；
    // 档位名由 format 参数画在框内。
    static char preset_buf[48];
    std::snprintf(preset_buf, sizeof(preset_buf), "%s", kJbPresets[s.preset_index].label);
    ImGui::Text("网络预设");
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::SliderInt("##preset", &s.preset_index, 0, kPresetCount - 1, preset_buf)) {
        // 拖动即换档；改动于下次连接生效（JB 参数是连接属性）。
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s\n\n改动于下次连接生效", kJbPresets[s.preset_index].short_hint);
    }

    // ---- 自定义配置勾选框 + 高级参数按钮（用户 2026-10-16）----
    // 滑块下方靠右一排：[✓]自定义配置  [高级参数…]。按钮弹出「高级设置」占位窗
    // （Phase 0 的 JB 参数微调面板：握手间隔 / 客户端名 / UDP 端口覆盖，会话接入
    // 后落地）；勾选框本身当前只记录状态（界面骨架阶段）。
    // 同理用 set_cursor_right_x：上面是全宽滑条，SameLine 会把本排拽回滑条行。
    const float cc_cb_w = ImGui::GetFrameHeight() + st.ItemInnerSpacing.x
        + ImGui::CalcTextSize("自定义配置").x;
    const float adv_btn_w = ImGui::CalcTextSize("高级参数…").x + st.FramePadding.x * 2.0f;
    set_cursor_right_x(cc_cb_w + adv_btn_w + st.ItemInnerSpacing.x);
    ImGui::Checkbox("自定义配置", &s.custom_config);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("勾选 = 使用「高级参数」里的自定义抖动缓冲配置。\n当前为界面骨架阶段：仅记录勾选状态。");
    }
    ImGui::SameLine(0.0f, st.ItemInnerSpacing.x);
    if (ImGui::Button("高级参数…")) {
        s.show_advanced = true; // 复用视图菜单同一入口（draw_placeholder_window）
    }

    // ---- 连接 / 断开按钮（草图要求的"大号"：全宽 + 加高 + 大字）----
    // 用户 2026-10-16 反馈"连接看起来有点小"：按当前字号放大 1.25 倍
    // （随设置里的字号 / DPI 联动，PushFont 的 size 参数是逻辑像素），
    // 高度同步 46 → 50 容纳大字。ImGui 1.92 对非常规字号惰性烘焙，即时生效。
    ImGui::Dummy(ImVec2(0.0f, 4.0f));
    const bool connected = (s.phase == UiPhase::Connected);
    const bool clickable = s.demo; // Phase 0：连接逻辑未接入，仅演示模式可点
    if (!clickable) {
        ImGui::BeginDisabled();
    }
    ImGui::PushFont(ImGui::GetFont(), ImGui::GetFontSize() * 1.25f);
    if (ImGui::Button(connected ? "断  开" : "连  接", ImVec2(-FLT_MIN, 50.0f))) {
        toggle_connection(s);
    }
    ImGui::PopFont();
    if (!clickable) {
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("连接功能在下一阶段接入（当前为界面骨架）。\n可先用 aqua_client_cli 连接；或设 AQUA_GUI_DEMO=1 预览界面。");
        }
    }

    // ---- 分割线（用户 2026-10-16：原“会话详情小字 / 填写服务器地址后点击连接”
    // 整段删除，改为一条分隔线；概要状态看底部状态栏即可）----
    ImGui::Separator();

    // ---- 指标条 ----
    ImGui::Dummy(ImVec2(0.0f, 6.0f));
    draw_metrics_strip(s);

    ImGui::EndChild();
}

// ---- 运行日志面板 ----
void draw_log_panel(UiState& s)
{
    // 必须判返回值：窗口被压到没有剩余高度时 BeginChild 返回 false，此时若继续
    // 提交内容，控件会落到父窗口、随后的 EndChild 配对错乱（探针实测矮窗口下
    // "Must call EndChild() and not End()" / "Missing EndTabBar()" 刷屏，release
    // 构建里可能直接 assert 崩）。返回 false = 整个面板不可见，正合语义。
    // Borders：用户 2026-10-16 第三轮反馈"运行日志不像上面的连接，是带一个框
    // 的"——两个半区视觉语法保持一致：tab 坐在盒子顶边框上，盒内 14/12 内边距。
    // （曾为省 24px 去掉边框，用户不买账。Borders 会使 child padding 从 0 涨到
    // (14,12)——imgui.cpp:7770 的零 padding 规则要求边框为 0——这就是盒子的
    // 内边距本身，不是额外开销。）
    if (!ImGui::BeginChild("##log_panel", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders)) {
        return;
    }

    // ---- 工具栏（独立一行：级别过滤 / 日志字号 / 自动滚动 / 右侧清空）----
    // 曾经的坑：工具栏跟在 SeparatorText 后面 SameLine，控件骑在分隔线上，
    // 观感重叠。现在标签页自带分区标题，工具栏就是普通一行，不再叠放。
    // 用户 2026-10-16：在「级别」与「自动滚动」之间加“调整字体大小”（只作用于
    // 下面的日志输出框，与 UI 字号、指标条字号独立）。滑条同样走“独立 Text 标签
    // + 空标签滑块填充”的规避手法（1.92 滑条标签画在框右侧，见网络预设处注释）。
    static const char* const kLevelNames[] = { "Debug", "Info", "Warn", "Error" };
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("级别");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(96.0f);
    if (ImGui::BeginCombo("##log_level", kLevelNames[s.log_min_level])) {
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
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("字体大小");
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("调整日志输出框的字体大小（10–20 px，仅作用于日志列表）。");
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(88.0f);
    ImGui::SliderInt("##log_font", &s.log_font_size, 10, 20, "%d px");
    ImGui::SameLine();
    ImGui::Checkbox("自动滚动", &s.log_autoscroll);

    // 右侧：清空（右对齐，与工具栏同一行）。另存为按钮按用户 2026-10-16 要求
    // 从日志工具栏移除（保存能力留给后续会话层 / 设置入口，Phase 0 不暴露）。
    const ImGuiStyle& st = ImGui::GetStyle();
    const float cw = ImGui::CalcTextSize("清空").x + st.FramePadding.x * 2.0f;
    same_line_right(cw);
    if (ImGui::Button("清空")) {
        log_buffer().clear();
    }

    // ---- 日志列表（增量缓存：version 或级别变化才重新快照）----
    static std::vector<LogLine> cached_lines;
    static std::uint64_t cached_version = 1;
    static int cached_level = -1;
    LogBuffer& log = log_buffer();
    const std::uint64_t ver = log.version();
    // 字号变化也要重新快照：PushTextWrapPos(0) 的换行宽度按当前字体度量，
    // 缓存行是按旧字号切的，不失效会出现“拖了滑条日志不重排”。
    static int cached_font = -1;
    if (ver != cached_version || cached_level != s.log_min_level
        || cached_font != s.log_font_size) {
        cached_lines = log.snapshot(s.log_min_level);
        cached_version = ver;
        cached_level = s.log_min_level;
        cached_font = s.log_font_size;
    }

    // 工具栏与列表之间只留一个标准行距：工具栏最后一个控件（清空按钮）提交后，
    // ItemSize 已把光标落到下一行（行高 + ItemSpacing.y = 控件底 + 7px），列表
    // child 直接从当前光标开始即可。曾经的 NewLine() 会再按一行文本高度（23px）
    // 推进，总空隙 ~30px（用户 2026-10-16 反馈“级别和下面的日志间隔过大”）。
    // 列表区用原生 FrameBg 作底（比面板底色略浅，形成内嵌感）——只复用
    // ImGuiCol_* 原生色，不引入自定义颜色。
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::GetStyleColorVec4(ImGuiCol_FrameBg));
    const float list_h = ImGui::GetContentRegionAvail().y;
    if (list_h > 24.0f && ImGui::BeginChild("##log_lines", ImVec2(0.0f, list_h))) {
        // 日志列表字号用工具栏滑条的值（默认 14 = 原 mono_size 口径），与 UI
        // 字号、指标条等宽字号相互独立。
        ImGui::PushFont(s.font_mono, static_cast<float>(s.log_font_size));
        ImGui::PushTextWrapPos(0.0f);
        if (cached_lines.empty()) {
            ImGui::TextDisabled("（暂无日志）");
        }
        for (const LogLine& line : cached_lines) {
            ImGui::Text("[%s] %-5s %s", line.stamp.c_str(), level_tag(line.level), line.text.c_str());
        }
        ImGui::PopTextWrapPos();
        ImGui::PopFont();
        if (s.log_autoscroll) {
            ImGui::SetScrollHereY(1.0f);
        }
        ImGui::EndChild();
    }
    ImGui::PopStyleColor();

    ImGui::EndChild();
}

// ---- 底部状态栏（重要信息常驻：状态 / 数据源 / 主题与版本）----
void draw_status_bar(const UiState& s)
{
    // 状态点：复用原生色（CheckMark = 绿），不写死色值，三个主题自适应。
    ImVec4 dot_col = ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
    if (s.phase == UiPhase::Connected) {
        dot_col = ImGui::GetStyleColorVec4(ImGuiCol_CheckMark);
    } else if (s.phase == UiPhase::Connecting || s.phase == UiPhase::Disconnecting) {
        dot_col = ImGui::GetStyleColorVec4(ImGuiCol_Text);
    }
    ImGui::TextColored(dot_col, "● %s", phase_text(s.phase));

    ImGui::SameLine();
    if (s.phase == UiPhase::Connected) {
        ImGui::TextDisabled("· 数据源 %s:%s", s.rpc_ip, s.rpc_port);
    } else {
        ImGui::TextDisabled("· 数据源 --");
    }

    // 右侧：主题 · （演示）· 版本（与左侧同一行右对齐）
    char right[96];
    if (s.demo) {
        std::snprintf(right, sizeof(right), "%s · 演示模式 · Aqua Client v%s",
            theme_name(s.theme), AQUA_CLIENT_CLI_VERSION);
    } else {
        std::snprintf(right, sizeof(right), "%s · Aqua Client v%s",
            theme_name(s.theme), AQUA_CLIENT_CLI_VERSION);
    }
    same_line_right(ImGui::CalcTextSize(right).x);
    ImGui::TextDisabled("%s", right);
}

// ---- 设置窗口（模态弹出：ImHex 式外观设置：主题 / 字号 / 日志，全部即时生效）----
// 模态 = BeginPopupModal：imgui 自动给背景压暗（ImGuiCol_ModalWindowDimBg，
// NewFrame 里按 GetTopMostPopupModal 调 DimBgRatio）并拦截其他窗口的输入——
// 关闭前点不了主窗口的任何控件。p_open 与标题栏 X 按钮联动；自绘关闭按钮
// 需额外 CloseCurrentPopup()（只置 p_open 不会关掉当前弹出）。
void draw_settings_window(UiState& s)
{
    if (!s.show_settings) {
        return;
    }
    ImGui::OpenPopup("外观设置");
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(430.0f, 0.0f), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("外观设置", &s.show_settings,
            ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize)) {
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

        // ---- 连接 ----
        // 断线自动重连按用户 2026-10-16 要求从主界面连接卡片移至此处（主界面只留
        // 固定使用此设备）。恢复默认不含它——重连是连接行为不是外观。
        ImGui::SeparatorText("连接");
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

// ---- 附加窗口（模态弹出；Phase 0 占位：说明该面板随哪个阶段提供）----
void draw_placeholder_window(const char* title, bool* open, const char* message)
{
    if (!*open) {
        return;
    }
    ImGui::OpenPopup(title);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(400.0f, 0.0f), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal(title, open,
            ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextWrapped("%s", message);
        ImGui::PopTextWrapPos();
        ImGui::Dummy(ImVec2(0.0f, 6.0f));
        if (ImGui::Button("知道了", ImVec2(-FLT_MIN, 0.0f))) {
            *open = false;
            ImGui::CloseCurrentPopup();
        }
        // EndPopup 只在 BeginPopupModal 返回 true 时调用（false 时内部已配对，
        // 原因见 draw_settings_window 内的注释）。
        ImGui::EndPopup();
    }
}

} // namespace

// ---- LogBuffer ----
void LogBuffer::append(int level, std::string text)
{
    LogLine line;
    line.stamp = now_hms();
    line.level = level;
    line.text = std::move(text);
    std::lock_guard<std::mutex> lock(mutex_);
    if (lines_.size() >= kCapacity) {
        lines_.pop_front();
    }
    lines_.push_back(std::move(line));
    ++version_;
}

void LogBuffer::clear()
{
    std::lock_guard<std::mutex> lock(mutex_);
    lines_.clear();
    ++version_;
}

std::vector<LogLine> LogBuffer::snapshot(int min_level) const
{
    std::vector<LogLine> out;
    std::lock_guard<std::mutex> lock(mutex_);
    out.reserve(lines_.size());
    for (const LogLine& line : lines_) {
        if (line.level >= min_level) {
            out.push_back(line);
        }
    }
    return out;
}

bool LogBuffer::save_to_file(const std::string& path) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (std::FILE* f = std::fopen(path.c_str(), "w"); f != nullptr) {
        for (const LogLine& line : lines_) {
            std::fprintf(f, "[%s] %s\n", line.stamp.c_str(), line.text.c_str());
        }
        std::fclose(f);
        return true;
    }
    return false;
}

std::uint64_t LogBuffer::version() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return version_;
}

LogBuffer& log_buffer()
{
    static LogBuffer buffer;
    return buffer;
}

// ---- 连接 / 断开 ----
// 连接大按钮、菜单 文件→连接/断开、Ctrl+K 快捷键共用本函数，避免两处状态机
// 漂移。定义在匿名命名空间外：main.cpp 的快捷键处理要调用它（见 ui_main.h）。
void toggle_connection(UiState& s)
{
    const bool connected = (s.phase == UiPhase::Connected);
    if (!s.demo) {
        log_buffer().append(kLogWarn,
            "连接功能在下一阶段接入（当前为界面骨架）；可设 AQUA_GUI_DEMO=1 预览交互。");
        return;
    }
    s.phase = connected ? UiPhase::Idle : UiPhase::Connected;
    s.demo_connected_at = ImGui::GetTime();
    log_buffer().append(kLogInfo, connected ? "演示：已断开（拟真数据）" : "演示：已连接（拟真数据）");
}

// ---- 主窗口 ----
// 布局（ImHex 式三段）：
//   菜单栏（viewport 级，BeginMainMenuBar）
//   └ 主窗口（从菜单栏下缘起，撑满剩余工作区）
//     ├ 内容 child（底边预留状态栏高度）
//     │  ├ 上半：「连接」单项 tab + 连接卡片（AutoResizeY，占自然高度）
//     │  ├ Separator（上下分屏的分割线）
//     │  └ 下半：「运行日志」单项 tab + 日志面板（吃满剩余高度）
//     ├ Separator
//     └ 状态栏（状态 · 数据源 ……………… 主题 · 版本）
void draw_main_window(UiState& s)
{
    static DeviceCache devices;
    refresh_devices(devices);

    static bool demo_seeded = false;
    if (s.demo && !demo_seeded) {
        demo_seeded = true;
        s.phase = UiPhase::Connected;
        s.demo_connected_at = ImGui::GetTime();
        seed_demo_logs();
    }

    // 菜单栏贴在 viewport 顶（独立窗口）；主窗口从它下缘开始，避免重叠。
    draw_menu_bar(s);

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    // 主视口工作区已被 imgui 自动扣掉主菜单栏：BeginMainMenuBar →
    // BeginViewportSideBar 把栏高累加进 viewport->BuildWorkInsetMin
    // （imgui_widgets.cpp:9314），次帧 UpdateViewportsNewFrame 才并进
    // WorkPos/WorkSize（imgui.cpp:16133）。所以直接贴 WorkPos/WorkSize 就是
    // "紧贴菜单栏下缘、撑满剩余工作区"，不需要再自己加菜单栏高度。
    // 曾经的 bug：无条件 vp->WorkPos.y + GetFrameHeight()——从第 1 帧起菜单栏
    // 被扣两次，主窗顶比菜单栏底缘低一整个 FrameHeight（26px 全黑空隙，用户
    // 截图 57~83 行实测"菜单栏和 tab 之间有一个空行"），窗口还顺带矮 26px。
    // 首帧扣除尚未生效（WorkPos.y 还是 0），取 max 兼容两种状态。
    const float menu_bar_h = ImGui::GetFrameHeight();
    const float top = (vp->WorkPos.y > menu_bar_h) ? vp->WorkPos.y : menu_bar_h;
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x, top));
    ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x, vp->WorkPos.y + vp->WorkSize.y - top));
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize
        | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar
        | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;
    // 菜单栏与主窗口之间不留空行：Begin 会把 style.WindowPadding 锁进
    // window->WindowPadding（imgui.cpp:7769），所以只对 Begin 这一下压成
    // (padding.x, 0) 再立即弹栈——顶 / 底都贴边；卡片与日志面板等子窗口
    // 在弹栈之后才创建，padding 不受影响。
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
        ImVec2(ImGui::GetStyle().WindowPadding.x, 0.0f));
    ImGui::Begin("##aqua_main", nullptr, flags);
    ImGui::PopStyleVar();

    // 内容区 = 除状态栏外的全部高度；负尺寸 = 距底边预留 chrome_h。
    // chrome_h 多留一行文本高度，容纳 Separator 与行距，防止状态栏被挤出底缘。
    const float chrome_h = ImGui::GetFrameHeight() + ImGui::GetTextLineHeightWithSpacing();
    ImGui::BeginChild("##content", ImVec2(0.0f, -chrome_h), ImGuiChildFlags_None);

    // 总可用高度要在加顶部间距之前取（间距之后 GetContentRegionAvail 会少算
    // 一段），下面的限高判断与 conn_natural_h（含间距的实测值）才同基准。
    const float content_avail_h = ImGui::GetContentRegionAvail().y;

    // 菜单栏与 tab 条之间留一个标准块间距（用户 2026-10-16 第三轮反馈："连接和
    // 上面的菜单栏中间没有间隔"）。主窗口的 WindowPadding.y 仍压成 0（否则底部
    // 状态栏也会被顶离底缘），只给内容区顶部加一个 WindowPadding.y 的偏移——
    // 底部不受影响，状态栏依旧贴底。
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + ImGui::GetStyle().WindowPadding.y);

    // ---- 上下分屏（用户 2026-10-16 设计变更）----
    // 旧结构：一个 TabBar 里「连接 | 运行日志」横排、点选切换。新结构：上下
    // 两半同时可见——上半连接、下半运行日志，中间 Separator 作分割线。每一半
    // 用「单项 tab bar」承载 tab 外观：imgui 对唯一 tab 会自动选中，内容每帧
    // 稳定可见（探针实测连续 4 帧 visible=1），不需要 SetSelected，也不会出现
    // 当年"每帧 SetSelected 把用户选择拽回旧 tab"的 bug。
    // 随之取消：request_tab / active_tab 编程切换（两个面板都常驻，没有
    // "当前页"可切）、Ctrl+L 快捷键、视图菜单的两个切页项；Ctrl+K 改回真正的
    // "连接 / 断开"（与文件菜单里 Ctrl+K 的标注一致，见 main.cpp）。
    //
    // tab 与内容贴贴（用户 2026-10-16 第二轮截图反馈："连接的 tab 和下面的框
    // 感觉是分开的，中间有一个小小的 margin"）：BeginTabItem 把内容光标落在
    // BarRect.Max.y + ItemSpacing.y（7px），tab 与内容因此裂开一条缝。把内容
    // 光标上移一个 ItemSpacing.y，同时把两个 tab bar 的 TabBarBorderSize 都压成
    // 0——tab 的方形下沿直接坐在盒子顶边框上（folder tab 观感），accent 分隔线
    // 由盒子边框接管，不叠成双线。两个半区视觉语法完全一致：tab + 边框盒。
    const float spacing_y = ImGui::GetStyle().ItemSpacing.y;

    // 连接半区的自然高度（顶部间距 12 + tab 26 + 卡片 AutoResizeY）随字号 /
    // 宽度变化，无法预算；用上一帧实测值判断本轮空间是否够（首帧估值，一帧后
    // 准确）。conn_natural_h 与 content_avail_h 同基准（都从内容 child 顶缘量、
    // 都含顶部间距）：
    //   够   → 不限高：卡片占自然高度，剩余全给日志半区，中间不留死空间；
    //   不够 → 连接半区限高到 (avail - 日志半区下限)，超高时内部滚动，
    //           日志半区始终可见（探针实测 620x460 客户区下布局不散、零 error）。
    static float conn_natural_h = 435.0f;
    constexpr float kMinLogHalfH = 180.0f; // 日志 tab(26) + 面板(~145) + 分割线(9)
    const bool cap_conn = (content_avail_h - conn_natural_h) < kMinLogHalfH;
    if (cap_conn) {
        float h = content_avail_h - kMinLogHalfH;
        if (h < 160.0f) {
            h = 160.0f; // 极端矮窗口：连接半区也不至于没有
        }
        ImGui::BeginChild("##conn_half", ImVec2(0.0f, h), ImGuiChildFlags_None);
    }
    ImGui::PushStyleVar(ImGuiStyleVar_TabBarBorderSize, 0.0f);
    if (ImGui::BeginTabBar("##conn_tab")) {
        if (ImGui::BeginTabItem("连接")) {
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() - spacing_y);
            draw_connection_card(s, devices);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::PopStyleVar();
    if (cap_conn) {
        ImGui::EndChild();
    } else {
        // 实测自然高度要减掉一个 ItemSpacing.y：卡片 child 提交后 ItemSize 会把
        // 光标多推进一个间距，EndTabBar 的 CurrTabsContentsHeight 含这段——不减
        // 的话估值比真实占用高 7px，默认窗口下会误触发限高包裹，卡片底与分割线
        // 之间露出死空间（2026-10-16 实测 17px，用户对这个敏感）。
        conn_natural_h = ImGui::GetCursorPosY() - ImGui::GetStyle().ItemSpacing.y;
    }

    // 中间分割线（用户原话："中间一个分割线，上面一个 tab，下面一个 tab"）。
    ImGui::Separator();

    // 下半：运行日志。日志面板是 Borders 盒子（与连接卡片一致的视觉语法），
    // 尺寸 (0,0) = 吃满剩余高度，窗口拉高时日志区自动变高。同样把内容光标上移
    // 一个 ItemSpacing.y + TabBarBorderSize 归零：tab 坐在盒子顶边框上。
    ImGui::PushStyleVar(ImGuiStyleVar_TabBarBorderSize, 0.0f);
    if (ImGui::BeginTabBar("##log_tab")) {
        if (ImGui::BeginTabItem("运行日志")) {
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() - spacing_y);
            draw_log_panel(s);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::PopStyleVar();

    ImGui::EndChild();

    ImGui::Separator();
    draw_status_bar(s);

    ImGui::End();

    draw_settings_window(s);
    draw_about_window(s);
    draw_placeholder_window("诊断", &s.show_diagnostics,
        "诊断面板（缓冲 / 传输 / 网络 / 音质指标与实时曲线）随会话功能一起提供。"
        "当前为界面骨架阶段：连接逻辑接入后，这里会显示与 CLI / Android 一致的实时诊断。");
    draw_placeholder_window("高级设置", &s.show_advanced,
        "高级设置（网络预设微调 / 握手间隔 / 客户端名 / UDP 端口覆盖）随会话功能一起提供。"
        "卡片里的网络预设滑条现在即可切换，改动于下次连接生效。");
}

} // namespace aqua::gui
