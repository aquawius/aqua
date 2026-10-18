#include "ui_panels.h"

#include "client_session.h"
#include "app_style.h"
#include "jb_presets.h"
#include "log_buffer.h"
#include "ui_layout.h"
#include "ui_main.h"
#include "ui_state.h"

#include <aqua/audio/devices/audio_device_manager.h>
#include <aqua/runtime/client_runtime.h>

#include <imgui.h>

#include <algorithm>
#include <cfloat>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace aqua::gui {
namespace {

// 播放设备 / 网络预设 / 指标条的数据源分别见 DeviceCache（本文件）、
// jb_presets.*（预设表与详细参数）、ui_metrics.cpp（指标口径）。

// ---- 播放设备下拉的数据源 ----
// 设备 ID 跨会话不稳定（WASAPI endpoint id 每次运行可能变），列表只作当期选择、
// 不持久化；index 0 固定"系统默认（跟随）"。每 2s 刷新一次（设备枚举自带 COM
// 初始化、任意线程可调；这里就在 UI 线程做，频次很低）。
// ids 与 names 一一对应：names 给下拉显示，ids 给连接配置（AudioRoute::pin）。
struct DeviceCache {
    std::unique_ptr<aqua::audio::AudioDeviceManager> manager;
    std::vector<std::string> names;
    std::vector<aqua::audio::AudioDeviceId> ids;
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
        cache.ids.clear();
        cache.names.emplace_back("系统默认（跟随）");
        cache.ids.emplace_back(); // 空 id = 平台默认设备（AudioRoute::follow_system）
        if (cache.manager == nullptr) {
            cache.platform_supported = false;
            cache.names.emplace_back("（当前平台没有音频后端）");
            cache.ids.emplace_back();
            return;
        }
        cache.platform_supported = true;
        for (const aqua::audio::AudioDevice& dev : cache.manager->enumerate(aqua::audio::AudioDeviceDirection::OUTPUT)) {
            cache.names.push_back(dev.name);
            cache.ids.push_back(dev.id);
        }
    } catch (const std::exception& e) {
        cache.platform_supported = false;
        cache.names.clear();
        cache.ids.clear();
        cache.names.emplace_back("系统默认（跟随）");
        cache.ids.emplace_back();
        cache.names.emplace_back(std::string("（设备枚举失败：") + e.what() + "）");
        cache.ids.emplace_back();
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

// ---- 从 UI 状态组装一次连接配置 ----
// 端口解析在这里，端点自洽性交给 core 的 validate_client_endpoint——通配地址、
// v4/v6 族不匹配这类判定只有 core 有权威口径（CLI 也只做消息转译，不另写一套）。
// 失败返回 false 并给出可直接展示给用户的原因。
bool build_session_config(const UiState& s, const DeviceCache& devices,
    SessionConfig& out, std::string& error)
{
    char* end = nullptr;
    const long rpc_port = std::strtol(s.rpc_port, &end, 10);
    if (end == s.rpc_port || *end != '\0' || rpc_port <= 0 || rpc_port > 65535) {
        error = "gRPC 端口非法（应为 1–65535）";
        return false;
    }
    out.rpc_ip = s.rpc_ip;
    out.rpc_port = static_cast<std::uint16_t>(rpc_port);
    out.client_name = s.client_name;

    out.udp_force_port.reset();
    if (s.udp_port[0] != '\0') {
        const long udp_port = std::strtol(s.udp_port, &end, 10);
        if (end == s.udp_port || *end != '\0' || udp_port <= 0 || udp_port > 65535) {
            error = "UDP 端口非法（留空 = 跟随 Server，或 1–65535）";
            return false;
        }
        out.udp_force_port = static_cast<std::uint16_t>(udp_port);
    }

    aqua::runtime::ClientRuntimeConfig probe; // 只为跑 core 的两条校验，不用于连接
    probe.rpc_ip = out.rpc_ip;
    probe.rpc_port = out.rpc_port;
    if (auto why = aqua::runtime::validate_client_endpoint(probe); why.has_value()) {
        error = *why;
        return false;
    }
    // 客户端名同理：服务端 Connect 会硬拒空名与超长名，协议约束只有 core 有权威口径。
    probe.client_name = out.client_name;
    if (auto why = aqua::runtime::validate_client_identity(probe); why.has_value()) {
        error = *why;
        return false;
    }

    // 设备钉住：只有选了具体设备才有 id；index 0（系统默认）本身就是跟随。
    out.pin_device = s.pin_device && s.device_index > 0
        && s.device_index < static_cast<int>(devices.ids.size());
    out.device_id = out.pin_device ? devices.ids[static_cast<std::size_t>(s.device_index)]
                                   : aqua::audio::AudioDeviceId {};

    out.auto_reconnect = s.auto_reconnect;
    out.reconnect_delay_s = std::clamp(s.reconnect_delay_s, 1, 10);
    // JB 参数：自定义配置（原始值 0 = core 默认）或预设档位解析，两者殊途同归
    // —— jb_effective 保证 0 字段一律回落 core 默认，会话层拿到的永远是完整值。
    out.jb = s.custom_config ? jb_effective(s.jb_custom) : jb_resolve(s.preset_index);
    // 其余连接属性（设置 → 高级；CLI 同名开关）。
    out.jb_adaptive_target = s.jb_adaptive_target;
    out.jb_concealment = s.jb_concealment;
    out.jb_splice = s.jb_splice;
    out.jb_packet_trace = s.jb_packet_trace;
    out.playback_buffer_frames = s.playback_buffer_frames;
    return true;
}

} // namespace

// ---- 连接 / 断开 ----
// 连接大按钮与菜单 文件→连接/断开共用本函数，避免两处状态机漂移。
// 两条路：
//   演示模式（AQUA_GUI_DEMO）  —— 不碰 core，只切一个假 phase，用于无服务器预览界面；
//   真实模式                   —— 交给 ClientSession（worker 线程跑 ClientRuntime）。
// 阶段本身由会话层快照驱动（main 循环每帧 apply_session_snapshot），本函数只在
// 演示路径下手动改 phase —— 单一事实来源，不两头写。
void toggle_connection(UiState& s)
{
    s.action_error.clear(); // 每次操作都清掉上一次的校验错误

    if (s.demo) {
        const bool connected = (s.phase == UiPhase::Connected);
        s.phase = connected ? UiPhase::Idle : UiPhase::Connected;
        s.connected_at = ImGui::GetTime();
        log_buffer().append(kLogInfo, connected ? "演示：已断开（拟真数据）" : "演示：已连接（拟真数据）");
        return;
    }

    if (s.phase == UiPhase::Connecting || s.phase == UiPhase::Disconnecting) {
        return; // 进行中：按钮同时是禁用的，这里双保险
    }
    if (s.phase == UiPhase::Connected) {
        client_session().disconnect();
        return;
    }

    // Idle / Failed → 组装配置并发起连接
    SessionConfig config;
    std::string error;
    if (!build_session_config(s, conn_panel_state().devices, config, error)) {
        s.action_error = error;
        log_buffer().append(kLogError, "无法发起连接：" + error);
        return;
    }
    if (!client_session().connect(config)) {
        log_buffer().append(kLogWarn, "已有会话在进行中，忽略本次连接请求。");
    }
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
    // BeginChild 返回 false = 本帧整个卡片被裁掉，跳过内容即可；但 **EndChild
    // 必须照调**：BeginChildEx 内部无条件 Begin()（imgui.cpp:6620）已经把子窗口
    // 压栈，提前 return 而不配对会让 ImGui 每帧报 "Must call EndChild() and not
    // End()"，随后整棵窗口树错乱（2026-10-18 探针实测：1040×460 矮窗口下爆发）。
    if (!ImGui::BeginChild("##connect_card", ImVec2(0.0f, 0.0f),
            ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY)) {
        ImGui::EndChild();
        return;
    }

    // ---- 连接期间锁定连接参数 ----
    // 地址 / 设备 / 网络预设都只在建连那一刻生效（JB 与路由都是连接属性），
    // 连接中改了不会生效。与其让控件"看起来能改"，不如明确锁住并给一行提示。
    // 演示模式不锁：那里没有真实会话，能自由操作控件正是预览界面的目的。
    // 连接中 / 已连接时锁住连接参数（地址 / 设备 / 网络预设都只在建连那一刻生效）。
    // 只置灰、不再加提示行：那行文字会把整张卡往下推一截，连接前后卡片高度不
    // 一致（用户 2026-10-18 反馈）。灰色 + tooltip 已足够表达"现在不能改"。
    const bool locked = !s.demo
        && (s.phase == UiPhase::Connecting || s.phase == UiPhase::Connected);
    if (!s.action_error.empty()) {
        // 校验失败用语义错误色（浅色主题下也看得清），而不是普通正文色。
        ImGui::PushStyleColor(ImGuiCol_Text, semantic_error());
        ImGui::TextWrapped("无法连接：%s", s.action_error.c_str());
        ImGui::PopStyleColor();
    }
    ImGui::BeginDisabled(locked);

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
        // 列宽比例 2.4 : 1 : 1.4（原 3 : 1 : 1.3）。双栏后卡片内宽从 564 降到 392，
        // 按 3:1:1.3 分只剩 224/75/97——UDP 列的占位提示 "留空 = 跟随 Server"
        // 被裁成 "留空 = 跟随 Se…"（2026-10-18 截图实测）。IP 列 224 对
        // "192.168.1.10"（≈96px）本就富余，匀 20px 给 UDP 列即恢复完整提示。
        ImGui::TableSetupColumn("ip", ImGuiTableColumnFlags_WidthStretch, 2.4f);
        ImGui::TableSetupColumn("rpc", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("udp", ImGuiTableColumnFlags_WidthStretch, 1.4f);

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
        // 占位提示从 "留空 = 跟随 Server" 缩成 "跟随 Server"：双栏后 UDP 列只有
        // ~103px 可用，原提示 ≈145px 被裁成 "留空 = 跟随 Se…"（截图实测）。
        // 完整规则挪进 tooltip，语义不丢。
        ImGui::InputTextWithHint("##udp_port", "跟随 Server", s.udp_port, sizeof(s.udp_port),
            ImGuiInputTextFlags_CharsDecimal);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("留空 = 采用 Server 通告的 UDP 端口（CLI 的 --udp-force-port）。\n填写 = 强制使用该本地端口。");
        }

        ImGui::EndTable();
    }

    // ---- 客户端名称（CLI 的 --client-name）----
    // 服务端日志里用来识别"是哪台机器在播"；协议约束（非空、≤128 字节）由 core 的
    // validate_client_identity 在发起连接时校验，这里只负责输入。输入框缓冲区长
    // 129 = 上限 + 1，天然截到 128 字节。
    // 标签与输入框**同行**：竖着排会多占一整行，把下半屏遥测面板压到装不下链路卡
    // （760 高的窗口下实测过：链路卡只剩标题）。
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("客户端名称");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##client_name", "aqua-client", s.client_name, sizeof(s.client_name));
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("服务器日志里显示本机的名字（CLI 的 --client-name）。\n"
                          "不能为空、最长 128 字节；改动于下次连接生效。");
    }

    // ---- 播放设备（index 0 = 系统默认）----
    // 连接中 / 断开中：设备只在建连那一刻定，锁住。
    // **已连接：故意不锁**——播放中点下拉就是立即切换输出设备（core 的
    // set_playback_device 走完整候选链 + 回滚，与 Android 的播放设备弹层同一语义）。
    // 之前桌面版这里一刀锁死：想换个音箱输出必须先断开再重连，白断一次音。
    const bool switching = (s.phase == UiPhase::Connected && !s.demo);
    if (switching) {
        ImGui::EndDisabled(); // 只放开设备这一段，后面的勾选框 / 预设仍锁着
    }
    ImGui::Text("播放设备");
    // ---- 当前输出（播放中：声音实际落在哪台设备上）----
    // 下拉框显示的是**请求**，这一行标注的是**实际**：降级 / 兜底 / 自动跟随时
    // 两者会不一样（core 的 RoutePolicy 决策，UI 只转述诊断快照）。
    // 挂在"播放设备"标签这一行的右端（本来是空的）：单独占一行会把遥测面板压到
    // 装不下链路卡。名字太长时按可用宽度裁掉右端，不推挤左侧标签。
    if (s.phase == UiPhase::Connected && !s.demo) {
        const auto& diag = s.session.diag;
        // 实际输出优先 stream 回读（start 成功时落盘 + 后端实时回读），其次路由
        // 意图里的请求设备；都空 = 跟随系统默认。
        std::string id = diag.stream.device_id.value();
        if (id.empty()) {
            id = diag.requested_device_id.value();
        }
        std::string name = "跟随系统默认";
        if (!id.empty()) {
            name = id; // 设备已不在列表（拔出）时显示 id 原文：比"音频设备"更有信息量
            for (std::size_t i = 0; i < devices.ids.size(); ++i) {
                if (devices.ids[i].value() == id) {
                    name = devices.names[i];
                    break;
                }
            }
        }
        const std::string text = "当前输出：" + name;
        const float text_w = ImGui::CalcTextSize(text.c_str()).x;
        const float right = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
        const float left_limit = ImGui::GetItemRectMax().x + 16.0f;
        const float x = std::max(left_limit, right - text_w);
        const bool clipped = (text_w > right - left_limit);
        ImGui::SameLine();
        const float line_y = ImGui::GetCursorScreenPos().y;
        ImGui::SetCursorScreenPos(ImVec2(x, line_y));
        if (clipped) {
            ImGui::PushClipRect(ImVec2(left_limit, line_y - 2.0f),
                ImVec2(right, line_y + ImGui::GetTextLineHeight() + 2.0f), true);
        }
        ImGui::TextDisabled("%s", text.c_str());
        if (clipped) {
            ImGui::PopClipRect();
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", text.c_str());
        }
    }
    ImGui::SetNextItemWidth(-FLT_MIN);
    const char* preview = devices.names[static_cast<std::size_t>(s.device_index)].c_str();
    if (ImGui::BeginCombo("##device", preview)) {
        for (int i = 0; i < static_cast<int>(devices.names.size()); ++i) {
            const bool selected = (i == s.device_index);
            if (ImGui::Selectable(devices.names[static_cast<std::size_t>(i)].c_str(), selected)) {
                s.device_index = i;
                // 切回「系统默认（跟随）」时把"钉住"一并取消：留着勾选状态会让
                // 下面那个勾选框显示为"已钉住但禁用"，自相矛盾。
                if (i == 0) {
                    s.pin_device = false;
                }
                // 播放中选择设备 = 立即切换，而不是只改下次连接的起步目标。
                // 候选链与回滚都在 core 里；切换失败时 UI 状态不动，实际输出设备
                // 由下一帧快照带回来（可能已是降级后的系统默认）。
                if (switching) {
                    if (i == 0) {
                        if (client_session().set_playback_device(std::nullopt)) {
                            log_buffer().append(kLogInfo, "播放设备：跟随系统默认");
                        }
                    } else {
                        s.pin_device = true; // 重连时仍以这台起步
                        const std::string name = devices.names[static_cast<std::size_t>(i)];
                        if (client_session().set_playback_device(
                                devices.ids[static_cast<std::size_t>(i)])) {
                            log_buffer().append(kLogInfo, "播放设备已切换：" + name);
                        } else {
                            log_buffer().append(kLogWarn,
                                "播放设备切换失败：" + name + "（详见运行日志）");
                        }
                    }
                }
            }
            if (selected) {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) {
        if (switching) {
            ImGui::SetTooltip("播放中：选择即立即切换输出设备（断开重连不需要）。");
        } else if (!devices.platform_supported) {
            ImGui::SetTooltip("当前平台没有音频后端（Linux / macOS 播放待实现）。\nWindows 上这里会列出全部输出设备。");
        } else {
            ImGui::SetTooltip("本次连接使用的输出设备；「系统默认」= 跟随系统默认输出。");
        }
    }

    if (switching) {
        ImGui::BeginDisabled(locked); // 恢复设备段之前的锁定状态
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
    // 选了"系统默认"时钉住无意义（本来就是跟随），禁用而不是让它看起来可点。
    const bool pin_meaningful = (s.device_index > 0);
    if (!pin_meaningful) {
        ImGui::BeginDisabled();
    }
    ImGui::Checkbox("固定使用此设备", &s.pin_device);
    if (!pin_meaningful) {
        ImGui::EndDisabled();
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        if (!pin_meaningful) {
            ImGui::SetTooltip("先在上面选一个具体设备，才能钉住它。");
        } else {
            ImGui::SetTooltip("勾选 = 钉住下拉所选设备（设备消失时按回退链处理）；\n不勾 = 跟随系统默认输出设备。");
        }
    }

    // ---- 网络预设滑条（11 档）：标签独立一行，滑条下一行 ----
    // 用户设计变更（2026-10-16）：标签不再与滑条挤在同一行。ImGui 1.92 的
    // 滑条把标签画在滑条框的右侧，若给滑条设填充宽度（-FLT_MIN）标签会被
    // 挤出内容区右缘遭裁切，因此标签用独立 Text、滑条用空标签填充剩余宽度；
    // 档位名由 format 参数画在框内。
    // 悬浮提示（用户 2026-10-17："希望变得更详细"）：由 jb_presets 生成完整
    // 参数表（6 项 + 哪些是 core 默认），与设置→高级 tab 同一数据源。
    // 勾了「自定义配置」时整条置灰：档位不再生效，继续能拖只会误导。
    ImGui::Text("网络预设");
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (!s.custom_config) {
        if (ImGui::SliderInt("##preset", &s.preset_index, 0, kJbPresetCount - 1, jb_preset_label(s.preset_index))) {
            // 拖动即换档；改动于下次连接生效（JB 参数是连接属性）。
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s\n\n改动于下次连接生效", jb_preset_tooltip(s.preset_index).c_str());
        }
    } else {
        ImGui::BeginDisabled();
        int preset_ro = s.preset_index; // 置灰但仍显示当前档位名
        ImGui::SliderInt("##preset", &preset_ro, 0, kJbPresetCount - 1, jb_preset_label(s.preset_index));
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("已勾选「自定义配置」：抖动缓冲改用高级参数里的值，\n上面的网络预设档位不再生效。");
        }
    }

    // ---- 自定义配置勾选框 + 高级参数按钮（用户 2026-10-16）----
    // 滑块下方靠右一排：[✓]自定义配置  [高级参数…]。按钮直接把设置框拉到
    // 「高级」tab（用户 2026-10-17：不再单开一个占位窗，统一进设置框）。
    // 同理用 set_cursor_right_x：上面是全宽滑条，SameLine 会把本排拽回滑条行。
    const float cc_cb_w = ImGui::GetFrameHeight() + st.ItemInnerSpacing.x
        + ImGui::CalcTextSize("自定义配置").x;
    const float adv_btn_w = ImGui::CalcTextSize("高级参数…").x + st.FramePadding.x * 2.0f;
    set_cursor_right_x(cc_cb_w + adv_btn_w + st.ItemInnerSpacing.x);
    // 走 set_custom_config（不直接绑字段）：开启那一刻把当前档位的值抄进
    // jb_custom，高级 tab 的滑条从用户刚看过的档位起步，而不是 core 默认。
    bool custom = s.custom_config;
    if (ImGui::Checkbox("自定义配置", &custom)) {
        set_custom_config(s, custom);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("勾选 = 在「高级」里逐项微调抖动缓冲参数，\n上面的网络预设档位置灰（不再生效）。\n改动于下次连接生效。");
    }
    ImGui::SameLine(0.0f, st.ItemInnerSpacing.x);
    if (ImGui::Button("高级参数…")) {
        s.settings_tab = kSettingsAdvanced; // 请求位：设置框这次直接落高级 tab
        s.show_settings = true;
    }

    // 连接参数区到此为止：解除整段禁用（下面的按钮自己管可用性）。
    ImGui::EndDisabled();

    // ---- 连接 / 断开按钮（草图要求的"大号"：全宽 + 加高 + 大字）----
    // 用户 2026-10-16 反馈"连接看起来有点小"：按当前字号放大 1.25 倍
    // （随设置里的字号 / DPI 联动，PushFont 的 size 参数是逻辑像素），
    // 高度同步 46 → 50 容纳大字。ImGui 1.92 对非常规字号惰性烘焙，即时生效。
    // 可用性：连接中 / 断开中禁用（避免重复点击把状态机搅乱）；其余状态可点
    // ——演示模式切假 phase，真实模式走 ClientSession。
    ImGui::Dummy(ImVec2(0.0f, 4.0f));
    const bool connected = (s.phase == UiPhase::Connected);
    const bool busy = (s.phase == UiPhase::Connecting || s.phase == UiPhase::Disconnecting);
    if (busy) {
        ImGui::BeginDisabled();
    }
    ImGui::PushFont(ImGui::GetFont(), ImGui::GetFontSize() * 1.25f);
    if (ImGui::Button(connected ? "断  开" : "连  接", ImVec2(-FLT_MIN, 50.0f))) {
        toggle_connection(s);
    }
    ImGui::PopFont();
    if (busy) {
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip(s.phase == UiPhase::Connecting ? "正在连接…（详见运行日志）"
                                                             : "正在断开…");
        }
    }

    // 卡片到此结束（用户 2026-10-18：下面的分隔线与指标条整段删除——指标表
    // 已挪进下方的遥测面板，卡片只负责"填地址 → 点连接"，高度因此缩到 ~320px，
    // 给可视化留出更多纵向空间）。

    ImGui::EndChild();
}

} // namespace aqua::gui
