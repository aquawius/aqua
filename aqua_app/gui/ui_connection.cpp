#include "ui_panels.h"

#include "log_buffer.h"
#include "ui_demo.h"
#include "ui_layout.h"
#include "ui_main.h"
#include "ui_state.h"

#include <aqua/audio/devices/audio_device_manager.h>

#include <imgui.h>

#include <cfloat>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace aqua::gui {
namespace {

// 等宽字号 = UI 基准字号 × 0.875（16 → 14，与早期手工调校值一致）。
// 注意 PushFont 接收的是"未缩放"基准值：style.FontScaleDpi 会统一再乘，
// 所以这里直接用 UiState.font_size 推导，不掺 DPI。
// 仅指标条数值用；日志列表字号是独立的（UiState::log_font_size）。
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

// ---- 连接面板的 UI 线程私有状态 ----
// 派生/缓存性质，不属于 UiState（丢了重建即可）。集中具名放这里，而不是散落
// 成函数内 static——接线的人改这个文件时能一眼看全本面板的全部隐藏状态。
struct ConnectionPanelState {
    DeviceCache devices;
};

ConnectionPanelState& conn_panel_state()
{
    static ConnectionPanelState state;
    return state;
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

} // namespace

// ---- 连接 / 断开 ----
// 连接大按钮、菜单 文件→连接/断开、Ctrl+K 快捷键共用本函数，避免两处状态机
// 漂移。Phase 0 只切演示态的 phase；会话接入后由 session 层的状态机接管，
// 本函数改为转调 session 接口（UI 侧调用点不用动）。
void toggle_connection(UiState& s)
{
    const bool connected = (s.phase == UiPhase::Connected);
    if (!s.demo) {
        log_buffer().append(kLogWarn,
            "连接功能在下一阶段接入（当前为界面骨架）；可设 AQUA_GUI_DEMO=1 预览交互。");
        return;
    }
    s.phase = connected ? UiPhase::Idle : UiPhase::Connected;
    s.connected_at = ImGui::GetTime();
    log_buffer().append(kLogInfo, connected ? "演示：已断开（拟真数据）" : "演示：已连接（拟真数据）");
}

// ---- 连接卡片 ----
// 顺序即视觉顺序；每个区块的间距由 ImGui 行距 + Dummy 微调，改动时跑一遍
// gui_check 探针核对矩形（本文件历史上有两次"控件重叠"都出自行距假设错误）。
void draw_connection_card(UiState& s)
{
    ConnectionPanelState& view = conn_panel_state();
    refresh_devices(view.devices); // 2s 节流；面板自有数据源
    DeviceCache& devices = view.devices;

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
    char preset_buf[48];
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
        s.show_advanced = true; // 复用视图菜单同一入口（draw_placeholder_windows）
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

} // namespace aqua::gui
