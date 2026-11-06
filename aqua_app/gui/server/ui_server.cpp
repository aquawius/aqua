#include "ui_server.h"

#include "app_style.h"
#include "log_buffer.h"
#include "server_settings.h"
#include "ui_format.h"
#include "ui_layout.h"

#include <aqua/audio/audio_error.h>
#include <aqua/audio/audio_format.h>
#include <aqua/audio/devices/audio_device_manager.h>
#include <aqua/audio/devices/audio_route.h>
#include <aqua/logger/logger.h>
#include <aqua/runtime/runtime_state.h>

#include <aqua_app/cli/cli_version.h>

#include <asio.hpp>
#include <imgui.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

// 服务端 GUI 的编排层。
//
// 版式（2026-11-06 第三轮，与 client 同构）：
//   菜单栏（文件 / 设置 / 帮助）
//   └ 主窗口
//     ├ tab：服务 | 运行日志
//     │   ├ 服务：参数卡片（地址 / 端口 / 逐族勾选 / 采集设备 / 大按钮）
//     │   │        + 运行信息卡（音频格式一行 + 四行采集读数，一列对齐）
//     │   │        + 会话卡（吃满剩余高度，表内滚动）
//     │   └ 日志页：与 client 完全相同的日志面板
//     └ 状态栏（● 阶段 · 控制端点 · 数据面实际绑定 ……………… 运行时长）
//
// 统计与高级设置都在「设置」模态里（外观 / 统计 / 高级），与 client 的三个 tab
// 一一对应：主页面只留"一眼要看到的东西"，明细不占常驻版面。
//
// 三个刻意的取舍：
//   1. 音频格式占**一行**（与 client 遥测面板的第一行同形：灰标签 + 等宽格式串）：
//      它是"这一次运行在传什么"，一次运行内不变，占一整张表不值得。采集的四行
//      读数与它共用同一个值列（kValueX），竖着看是一条线。
//   2. 主页面不铺表格线（统计页才铺）：运行信息用"标签列 + 行距"分隔，会话卡只
//      画内部分隔线，外框唯一。两张框套在一起时那道 8px 的缝会显得像没对齐。
//   3. 所有"启动后才有"的读数都用「-」占位，几何不随阶段跳动。

namespace aqua::gui {
namespace {

struct EncodingChoice {
    aqua::audio::AudioEncoding encoding;
    const char* name;
};

// 顺序即下拉框顺序；默认下标 2 = PCM F32LE（core 的默认编码）。
constexpr EncodingChoice kEncodings[] = {
    { aqua::audio::AudioEncoding::PCM_S16LE, "PCM S16LE" },
    { aqua::audio::AudioEncoding::PCM_S32LE, "PCM S32LE" },
    { aqua::audio::AudioEncoding::PCM_F32LE, "PCM F32LE" },
    { aqua::audio::AudioEncoding::PCM_S24LE, "PCM S24LE" },
    { aqua::audio::AudioEncoding::PCM_U8, "PCM U8" },
};
constexpr int kEncodingCount = static_cast<int>(sizeof(kEncodings) / sizeof(kEncodings[0]));

constexpr const char* kCaptureSources[] = { "系统输出（loopback）", "输入设备（麦克风）" };
constexpr int kCaptureSourceCount = 2;

constexpr const char* kSettingsTitle = "设置";

// 占位符：所有"启动后才有"的读数都用它（就是一个短横线），几何不随阶段跳动。
// 不要用整句话代替占位——那会让"这里本来该有个数"变成一段说明文字。
constexpr const char* kDash = "-";

// 运行信息卡的"标签列"宽度：音频格式行与下面四行读数共用同一个值列，
// 竖着看过去值是一条线（这是这张卡唯一的对齐轴，改宽度只会让两边一起动）。
// 96px = 最宽的标签「合成 / 事件」+ 20px 呼吸位。
constexpr float kValueX = 96.0f;

// 设备下拉第一项：跟随系统默认（与 client 的「系统默认（跟随）」同名）。
constexpr const char* kFollowSystemLabel = "系统默认（跟随）";

std::string fmt_text(const char* fmt, ...)
{
    // 512：最长的调用是采集行 tooltip（7 行 × 若干数字/单位），320 会截尾。
    char buf[512];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    return buf;
}

int encoding_index_of(aqua::audio::AudioEncoding encoding)
{
    for (int i = 0; i < kEncodingCount; ++i) {
        if (kEncodings[i].encoding == encoding) {
            return i;
        }
    }
    return 2; // 未知编码按默认显示
}

ImVec4 phase_color(ServerPhase phase)
{
    switch (phase) {
    case ServerPhase::Running:
        return ImGui::GetStyleColorVec4(ImGuiCol_CheckMark);
    case ServerPhase::Failed:
        return semantic_error();
    case ServerPhase::Starting:
    case ServerPhase::Stopping:
        return semantic_warn();
    case ServerPhase::Idle:
    default:
        return ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
    }
}

const char* capture_state_text(aqua::audio::AudioCaptureState state)
{
    using aqua::audio::AudioCaptureState;
    switch (state) {
    case AudioCaptureState::Active:  return "采集中";
    case AudioCaptureState::Silent:  return "静音";
    case AudioCaptureState::Starved: return "无数据";
    }
    return "未知";
}

// ---- 采集设备缓存 ----
// 枚举是毫秒级的系统调用（WASAPI 实测 5–8ms / 6 个设备），只在**停止时**每 3s
// 刷一次；运行中表单禁用，不枚举。
struct DeviceCache {
    std::unique_ptr<aqua::audio::AudioDeviceManager> manager;
    double last_refresh = -100.0;
    int source = -1;
};

DeviceCache& device_cache()
{
    static DeviceCache cache;
    return cache;
}

bool parse_port(const char* text, std::uint16_t& out)
{
    if (text == nullptr || *text == '\0') {
        return false;
    }
    char* end = nullptr;
    const long value = std::strtol(text, &end, 10);
    if (end == text || *end != '\0' || value <= 0 || value > 65535) {
        return false;
    }
    out = static_cast<std::uint16_t>(value);
    return true;
}

// 只接受 IP 字面量（与 CLI 的 --rpc-ip 同一约定：不做 DNS）。
bool parse_ip_literal(const char* text, asio::ip::address& out)
{
    if (text == nullptr || *text == '\0') {
        return false;
    }
    asio::error_code ec;
    out = asio::ip::make_address(text, ec);
    return !ec;
}

void setup_main_window_rect()
{
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float menu_bar_h = ImGui::GetFrameHeight();
    const float top = (vp->WorkPos.y > menu_bar_h) ? vp->WorkPos.y : menu_bar_h;
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x, top));
    ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x, vp->WorkPos.y + vp->WorkSize.y - top));
}

// 表单 → ServerRuntimeConfig。失败时写 s.form_error 并返回 false。
// 只拦"一眼能看出错"的东西：端口越界、IP 不是字面量、一个族都没开；更深层的
// 校验交给 ServerRuntime（启动失败的原因会显示在状态行 / 运行日志里）。
bool build_config_from_form(ServerUiState& s, aqua::runtime::ServerRuntimeConfig& out)
{
    const auto fail = [&s](std::string message) {
        s.form_error = std::move(message);
        return false;
    };

    asio::ip::address rpc_address;
    if (!parse_ip_literal(s.rpc_ip, rpc_address)) {
        return fail("控制面地址必须是 IP 字面量（例如 0.0.0.0 / 127.0.0.1 / ::）");
    }
    std::uint16_t rpc_port = 0;
    if (!parse_port(s.rpc_port, rpc_port)) {
        return fail("控制面端口必须是 1–65535");
    }

    // 勾选框 = 是否服务该族。未勾选等价于 CLI 的 --udp-ipv4/-ipv6 none。
    if (!s.udp_ipv4_enabled && !s.udp_ipv6_enabled) {
        return fail("至少启用一个数据面族：勾选 IPv4 或 IPv6");
    }
    if (s.udp_ipv4_enabled) {
        asio::ip::address address;
        if (!parse_ip_literal(s.udp_ipv4, address) || !address.is_v4()) {
            return fail("IPv4 绑定地址不是合法的 IPv4 字面量（例如 0.0.0.0）；"
                        "不想服务 IPv4 请取消勾选");
        }
    }
    if (s.udp_ipv6_enabled) {
        asio::ip::address address;
        if (!parse_ip_literal(s.udp_ipv6, address) || !address.is_v6()) {
            return fail("IPv6 绑定地址不是合法的 IPv6 字面量（例如 ::）；"
                        "不想服务 IPv6 请取消勾选");
        }
    }
    std::uint16_t udp_port = 0;
    if (!parse_port(s.udp_port, udp_port)) {
        return fail("数据面端口必须是 1–65535");
    }

    // 从 CLI 配置出发，只覆盖表单控制的字段：--audio-queue-capacity /
    // --session-timeout / advertise 等参数仍然生效。
    out = s.config;
    out.rpc_ip = s.rpc_ip;
    out.rpc_port = rpc_port;
    out.udp_ipv4 = s.udp_ipv4_enabled ? s.udp_ipv4 : std::string();
    out.udp_ipv6 = s.udp_ipv6_enabled ? s.udp_ipv6 : std::string();
    out.udp_port = udp_port;
    out.frame_count = static_cast<std::uint32_t>(std::max(0, s.packet_frames));

    if (s.follow_format) {
        out.format.reset();
    } else {
        aqua::audio::AudioFormat format;
        format.encoding = kEncodings[std::clamp(s.encoding_index, 0, kEncodingCount - 1)].encoding;
        format.channels = static_cast<std::uint32_t>(std::clamp(s.channels, 1, 64));
        format.sample_rate = static_cast<std::uint32_t>(std::clamp(s.sample_rate, 1, 768000));
        if (!format.is_valid()) {
            return fail("音频格式不合法：声道 1–64、采样率 1–768000");
        }
        out.format = format;
    }

    out.capture.source = (s.capture_source == 1)
        ? aqua::audio::AudioCaptureSource::INPUT_DEVICE
        : aqua::audio::AudioCaptureSource::OUTPUT_LOOPBACK;
    out.capture.route = (s.pin_device && !s.pinned_device_id.empty())
        ? aqua::audio::AudioRoute::pin(aqua::audio::AudioDeviceId(s.pinned_device_id))
        : aqua::audio::AudioRoute::follow_system();
    out.capture.format.reset(); // 与 CLI 一致：采集侧用设备默认格式
    return true;
}

// ---- 表格 / 文本助手 ----
// 与 client 统计页同一套观感：左列固定宽标签，右列等宽数字。
bool begin_stat_table(const char* id, float label_width = 96.0f)
{
    if (!ImGui::BeginTable(id, 2,
            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchSame)) {
        return false;
    }
    ImGui::TableSetupColumn("key", ImGuiTableColumnFlags_WidthFixed, label_width);
    ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch, 1.0f);
    return true;
}

// 语义色是**按主题现算的值**（semantic_warn() 返回 ImVec4，不是引用），
// 而 stat_row 要的是一个指针。这里缓存到函数局部 static 再取址：调用点与使用点
// 之间没有任何其他地方改主题，安全且不用在每个调用点写一行局部变量。
const ImVec4* tone_warn()
{
    static ImVec4 color;
    color = semantic_warn();
    return &color;
}

const ImVec4* tone_error()
{
    static ImVec4 color;
    color = semantic_error();
    return &color;
}

void stat_row(const ServerUiState& s, const char* key, const std::string& value,
    const ImVec4* color = nullptr)
{
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::TextUnformatted(key);
    ImGui::TableNextColumn();
    const float mono_px = mono_size_for(s.font_size);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + mono_baseline_dy_for(s.font_mono, mono_px));
    ImGui::PushFont(s.font_mono, mono_px);
    if (color != nullptr) {
        ImGui::TextColored(*color, "%s", value.c_str());
    } else {
        ImGui::TextUnformatted(value.c_str());
    }
    ImGui::PopFont();
}

std::string device_label(const ServerUiState& s, const std::string& id)
{
    for (const auto& device : s.devices) {
        if (device.id == id) {
            return device.name;
        }
    }
    return id; // 设备已拔出：显示 id 原文，比"音频设备"有信息量
}

// ---- 音频格式行（与 client 遥测面板第一行同形）----
// 灰色标签 + 等宽格式串，一行读完；宽度不够时裁切，靠 tooltip 补全。
// 值本身由调用方给（未运行时是 "-" 占位），这里只负责排版，不判断阶段。
void draw_format_line(const ServerUiState& s, const std::string& value)
{
    ImGui::TextDisabled("音频格式");
    const float avail = std::max(48.0f, ImGui::GetWindowContentRegionMax().x - kValueX);

    const float mono_px = mono_size_for(s.font_size);
    const float dy = mono_baseline_dy_for(s.font_mono, mono_px);

    ImGui::SameLine(kValueX);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + dy);

    ImGui::PushFont(s.font_mono, mono_px);
    const bool clipped = ImGui::CalcTextSize(value.c_str()).x > avail;
    ImGui::PopFont();

    if (clipped) {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::PushClipRect(p, ImVec2(p.x + avail, p.y + ImGui::GetTextLineHeight() + 2.0f), true);
    }
    ImGui::PushFont(s.font_mono, mono_px);
    ImGui::TextUnformatted(value.c_str());
    ImGui::PopFont();
    if (clipped) {
        ImGui::PopClipRect();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("服务端正在往外送的音频格式：采样率 · 声道 · 编码 · 码率 · 每包时长。\n"
                          "%s\n"
                          "客户端连上后自动跟随，不用手动配；想换格式去 设置 → 高级。",
            value.c_str());
    }
}

// ---- 读数行：标签灰、值等宽，值列与音频格式行对齐 ----
// 这张卡不用表格：四行读数 + 一行格式属于同一份"规格"，表格的横竖线只会把它
// 切成两半、还和外框凑成两层框。分隔交给行距与标签列。
// tip 非空时整行（标签或值）悬停都能看到解释——主页上这些数字对不熟悉的用户
// 不直观，提示要写成大白话（用户 2026-11-06："提示尽量通俗易懂"）。
void stat_line(const ServerUiState& s, const char* label, const std::string& value,
    const ImVec4* color = nullptr, const char* tip = nullptr)
{
    const float mono_px = mono_size_for(s.font_size);
    const float dy = mono_baseline_dy_for(s.font_mono, mono_px);

    ImGui::TextDisabled("%s", label);
    if (tip != nullptr && ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", tip);
    }
    ImGui::SameLine(kValueX);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + dy);
    ImGui::PushFont(s.font_mono, mono_px);
    if (color != nullptr) {
        ImGui::PushStyleColor(ImGuiCol_Text, *color);
    }
    ImGui::TextUnformatted(value.c_str());
    if (color != nullptr) {
        ImGui::PopStyleColor();
    }
    ImGui::PopFont();
    if (tip != nullptr && ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", tip);
    }
}

// ---- 服务参数卡片 ----
void draw_config_card(ServerUiState& s)
{
    const ServerSnapshot& snap = s.snapshot;
    const bool live = (snap.phase == ServerPhase::Running);
    const bool busy = (snap.phase == ServerPhase::Starting || snap.phase == ServerPhase::Stopping);
    const bool editable = !live && !busy;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0f, 10.0f));
    ImGui::BeginChild("##srv_config", ImVec2(0.0f, 0.0f),
        ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY);

    ImGui::BeginDisabled(!editable);

    // ---- 控制地址 + 两个端口（标签一行、输入一行）----
    // 拆两行的理由与 client 连接卡一致：imgui 表格会把单元格里第一个 framed
    // 控件（InputText）的基线传播成整行基线，后续单元格的 Text 标签会被垫高
    // 一个 FramePadding.y，三列输入框因此不在一条水平线上。
    if (ImGui::BeginTable("##addr", 3, ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("addr", ImGuiTableColumnFlags_WidthStretch, 2.6f);
        ImGui::TableSetupColumn("rpc", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("udp", ImGuiTableColumnFlags_WidthStretch, 1.0f);

        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted("控制地址");
        ImGui::TableNextColumn();
        ImGui::TextUnformatted("控制端口");
        ImGui::TableNextColumn();
        ImGui::TextUnformatted("数据端口");

        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputTextWithHint("##rpc_ip", "0.0.0.0", s.rpc_ip, sizeof(s.rpc_ip));
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("gRPC 控制面绑定地址。0.0.0.0 / :: = 所有网卡（gRPC 对通配自带双栈）。");
        }
        ImGui::TableNextColumn();
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputText("##rpc_port", s.rpc_port, sizeof(s.rpc_port),
            ImGuiInputTextFlags_CharsDecimal);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("控制面（gRPC）端口，客户端「连接」页填的就是它。默认 50051。");
        }
        ImGui::TableNextColumn();
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputText("##udp_port", s.udp_port, sizeof(s.udp_port),
            ImGuiInputTextFlags_CharsDecimal);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("数据面 UDP 端口，IPv4 与 IPv6 共用。默认 50000。");
        }
        ImGui::EndTable();
    }

    // ---- 数据面：逐族勾选 + 绑定地址 ----
    // 勾选框 = 是否服务该族（等价 CLI 的 --udp-ipv4/-ipv6 none）。留空不再表示
    // "关闭"：两种表达方式并存时，界面上的空字符串到底算不算关，没人说得清。
    if (ImGui::BeginTable("##udp_bind", 2, ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("fam", ImGuiTableColumnFlags_WidthFixed, 78.0f);
        ImGui::TableSetupColumn("bind", ImGuiTableColumnFlags_WidthStretch, 1.0f);

        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::Checkbox("IPv4", &s.udp_ipv4_enabled);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("服务 IPv4 客户端。取消勾选 = 只开 IPv6（CLI 的 --udp-ipv4 none）。");
        }
        ImGui::TableNextColumn();
        ImGui::BeginDisabled(!s.udp_ipv4_enabled);
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputTextWithHint("##udp_v4", "0.0.0.0", s.udp_ipv4, sizeof(s.udp_ipv4));
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("IPv4 socket 的绑定地址。0.0.0.0 = 所有 IPv4 网卡。");
        }

        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::Checkbox("IPv6", &s.udp_ipv6_enabled);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("服务 IPv6 客户端（v6-only socket）。取消勾选 = 只开 IPv4。");
        }
        ImGui::TableNextColumn();
        ImGui::BeginDisabled(!s.udp_ipv6_enabled);
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputTextWithHint("##udp_v6", "::", s.udp_ipv6, sizeof(s.udp_ipv6));
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("IPv6 socket 的绑定地址。:: = 所有 IPv6 网卡。");
        }
        ImGui::EndTable();
    }

    // ---- 采集来源 + 采集设备（并排：同一个决定的两半）----
    // 下拉框永远表示**意图**（跟随系统 / 钉住某个 id）；运行中"实际打开的是哪个"
    // 由下面那行「当前捕获：…」回答——两者在跟随系统时本来就可能不同。
    std::string current_label = kFollowSystemLabel;
    if (s.pin_device && !s.pinned_device_id.empty()) {
        current_label = device_label(s, s.pinned_device_id);
    }

    if (ImGui::BeginTable("##cap_pick", 2, ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("src", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("dev", ImGuiTableColumnFlags_WidthStretch, 1.7f);

        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted("采集来源");
        ImGui::TableNextColumn();
        ImGui::TextUnformatted("采集设备");

        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::BeginCombo("##cap_source",
                kCaptureSources[std::clamp(s.capture_source, 0, kCaptureSourceCount - 1)])) {
            for (int i = 0; i < kCaptureSourceCount; ++i) {
                if (ImGui::Selectable(kCaptureSources[i], s.capture_source == i)) {
                    if (s.capture_source != i) {
                        s.capture_source = i;
                        // 方向变了，固定的设备就不再属于这个列表：退回跟随系统。
                        s.pin_device = false;
                        s.pinned_device_id.clear();
                    }
                }
            }
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("loopback 录系统输出（需要 OUTPUT 设备）；输入设备录麦克风。");
        }

        ImGui::TableNextColumn();
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::BeginCombo("##cap_device", current_label.c_str())) {
            if (ImGui::Selectable(kFollowSystemLabel, !s.pin_device)) {
                s.pin_device = false;
                s.pinned_device_id.clear();
            }
            for (const auto& device : s.devices) {
                if (device.id.empty()) {
                    continue;
                }
                const bool selected = s.pin_device && s.pinned_device_id == device.id;
                // 选具体设备 = 钉住它（与 client 一致：选完不用再勾一次）。
                if (ImGui::Selectable(device.name.c_str(), selected)) {
                    s.pin_device = true;
                    s.pinned_device_id = device.id;
                }
            }
            if (s.devices.empty()) {
                ImGui::TextDisabled("（未枚举到设备）");
            }
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered()) {
            if (live) {
                ImGui::SetTooltip("本次运行锁定的采集设备（启动前选的）；\n"
                                  "实际打开的是哪个见下面那行「当前捕获」。");
            } else if (s.devices.empty()) {
                ImGui::SetTooltip("没有枚举到该方向的设备：loopback 需要至少一个 OUTPUT 设备。");
            } else {
                ImGui::SetTooltip("选具体设备即钉住它；钉住的设备消失时 server 会直接停止\n"
                                  "（不做静默换源）。");
            }
        }
        ImGui::EndTable();
    }

    // ---- 固定使用此设备（右对齐；运行中把"当前捕获"写在它左边）----
    const ImGuiStyle& st = ImGui::GetStyle();
    const float pin_w = ImGui::GetFrameHeight() + st.ItemInnerSpacing.x
        + ImGui::CalcTextSize("固定使用此设备").x;

    if (live) {
        // 与 client 的「当前输出：…」同一形状：实际设备在左、勾选框贴右缘。
        std::string actual_id = snap.diag.capture_switch.active_device_id;
        if (actual_id.empty()) {
            actual_id = snap.diag.capture_switch.requested_device_id;
        }
        const std::string text = "当前捕获："
            + (actual_id.empty() ? std::string(kFollowSystemLabel) : device_label(s, actual_id));
        const float avail = std::max(60.0f,
            ImGui::GetContentRegionAvail().x - pin_w - st.ItemSpacing.x * 2.0f);
        const bool clipped = ImGui::CalcTextSize(text.c_str()).x > avail;
        if (clipped) {
            const ImVec2 p = ImGui::GetCursorScreenPos();
            ImGui::PushClipRect(p, ImVec2(p.x + avail, p.y + ImGui::GetTextLineHeight() + 2.0f),
                true);
        }
        ImGui::TextDisabled("%s", text.c_str());
        if (clipped) {
            ImGui::PopClipRect();
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", text.c_str());
        }
        ImGui::SameLine(0.0f, 0.0f);
        set_cursor_right_x(pin_w);
    } else {
        set_cursor_right_x(pin_w);
    }

    // 没选具体设备时"钉住"无对象可钉：禁用而不是让它看起来可点（点了也不生效）。
    const bool pin_possible = !s.pinned_device_id.empty();
    ImGui::BeginDisabled(!pin_possible);
    if (ImGui::Checkbox("固定使用此设备", &s.pin_device)) {
        if (!s.pin_device) {
            // 取消固定 = 真的跟随系统：下拉同步回到「系统默认（跟随）」，
            // 否则勾选框与下拉框的状态会互相矛盾。
            s.pinned_device_id.clear();
        }
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        if (!pin_possible) {
            ImGui::SetTooltip("先在上面选一个具体设备，才能钉住它。");
        } else {
            ImGui::SetTooltip("勾选 = 钉住下拉所选设备（设备消失则停止服务）；\n"
                              "不勾 = 跟随系统默认输出设备。");
        }
    }

    ImGui::EndDisabled();

    // ---- 大按钮：与 client 的连接按钮同一套位置语言（卡片底部、整行宽、较大字号）----
    const bool running = (live || snap.phase == ServerPhase::Starting);
    const char* action = running ? "停止服务" : "启动服务";
    ImGui::BeginDisabled(snap.phase == ServerPhase::Stopping);
    ImGui::PushFont(ImGui::GetFont(), ImGui::GetFontSize() * 1.25f);
    if (ImGui::Button(action, ImVec2(-FLT_MIN, 46.0f))) {
        toggle_server(s);
    }
    ImGui::PopFont();
    ImGui::EndDisabled();

    // ---- 错误行：在大按钮的下面开一行，不放在卡片顶部 ----
    // 表单校验失败（form_error）与运行时启动失败/降级终止（status_detail）共用
    // 这一行。放顶部会让整张卡变高、下面所有内容一起下沉——错误恰恰是"点按钮
    // 之后"发生的事，紧跟着按钮出现在语义上也顺。没有错误时这一行完全不占位。
    if (!s.form_error.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, semantic_error());
        ImGui::TextWrapped("无法启动：%s", s.form_error.c_str());
        ImGui::PopStyleColor();
    } else if (snap.phase == ServerPhase::Failed && !snap.status_detail.empty()) {
        // 启动失败/运行时降级终止的原因不能只藏在状态栏 tooltip 里：异常的当下
        // 这一行就是用户最想看的东西。
        ImGui::PushStyleColor(ImGuiCol_Text, semantic_error());
        ImGui::TextWrapped("%s", snap.status_detail.c_str());
        ImGui::PopStyleColor();
    }

    ImGui::EndChild();
    ImGui::PopStyleVar();
}

// ---- 运行信息：一份"这次运行在传什么、采得怎么样"的规格 ----
// 上半是会话契约（音频格式，一次运行内不变），下半是采集的四行读数。
// 整张卡只有一列对齐轴（kValueX）：值一律等宽、左缘齐平，扫一眼就是一条竖线；
// 不铺表格线——那是统计页的事，主页面要的是"读得下去"而不是"看得全"。
void draw_runtime_card(ServerUiState& s)
{
    const ServerSnapshot& snap = s.snapshot;
    const bool live = snap.live;
    const auto& d = snap.diag;
    const auto& r = snap.rates;

    // 统一的值/颜色包装：没在跑的时候一律是 "-"，不写句子、不写 0。
    const auto text = [live](std::string value) {
        return (live && !value.empty()) ? std::move(value) : std::string(kDash);
    };
    const auto tone = [live](bool condition, const ImVec4* color) {
        return (live && condition) ? color : nullptr;
    };

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0f, 10.0f));
    ImGui::BeginChild("##runtime_card", ImVec2(0.0f, 0.0f),
        ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY);

    const float btn_w = ImGui::CalcTextSize("统计…").x + ImGui::GetStyle().FramePadding.x * 2.0f;
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("运行信息");
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("这次运行在传什么、采得怎么样、发得出去吗。\n"
                          "下面每一行都可以悬停看解释。");
    }
    same_line_right(btn_w);
    if (ImGui::Button("统计…")) {
        s.settings_tab = kSettingsStats;
        s.show_settings = true;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("打开 设置 → 统计（会话 / 采集 / 队列 / 分发 / 网络的完整计数）");
    }
    ImGui::Separator();

    // ---- 音频格式（一行）----
    // 字段顺序与 client 的音频格式行一致：采样率 · 声道 · 编码 · 码率 · 帧长。
    std::string format_line;
    if (d.audio_format.sample_rate > 0) {
        format_line = format_sample_rate(static_cast<int>(d.audio_format.sample_rate)) + " · "
            + format_channels(static_cast<int>(d.audio_format.channels)) + " · "
            + kEncodings[encoding_index_of(d.audio_format.encoding)].name;
        const std::uint32_t bytes_per_frame = d.audio_format.frame_bytes();
        if (bytes_per_frame > 0) {
            format_line += fmt_text(" · %u kbps",
                bytes_per_frame * d.audio_format.sample_rate * 8 / 1000);
        }
        if (d.frame_count > 0) {
            format_line += fmt_text(" · %u 帧 ≈ ", d.frame_count)
                + format_ms(static_cast<double>(d.frame_count) * 1000.0
                    / static_cast<double>(d.audio_format.sample_rate));
        }
    }
    draw_format_line(s, text(format_line));

    // ---- 采集读数（四行；不设小标题，行名自己说得清）----
    std::string state = capture_state_text(d.capture.state);
    state += " · " + format_rate(r.capture_fps) + " 帧/秒";
    if (d.audio_format.sample_rate > 0) {
        state += "（期望 " + format_rate(d.audio_format.sample_rate) + "）";
    }
    const bool starved = (d.capture.state == aqua::audio::AudioCaptureState::Starved)
        || (d.capture.current_starved_ms > 0);
    stat_line(s, "状态", text(state), tone(starved, tone_error()),
        "采集端现在有没有在干活。\n"
        "采集中 = 正常在录；静音 = 有数据但全是静音（系统没在放声音）；\n"
        "无数据 = 拿不到音频（设备被占用、被拔掉或驱动异常）。\n"
        "后面括号是期望值：实测帧率应当和采样率差不多，差太多说明采集不稳。");

    std::string frames;
    if (d.capture.packet_frames_last > 0) {
        frames = fmt_text("块 %u 帧", d.capture.packet_frames_last);
        if (d.capture.packet_frames_min > 0) {
            frames += fmt_text("（%u–%u）", d.capture.packet_frames_min,
                d.capture.packet_frames_max);
        }
    }
    if (d.capture.endpoint_buffer_frames > 0) {
        if (!frames.empty()) {
            frames += " · ";
        }
        frames += fmt_text("端点 %u 帧", d.capture.endpoint_buffer_frames);
        if (d.audio_format.sample_rate > 0) {
            frames += "（" + format_ms(static_cast<double>(d.capture.endpoint_buffer_frames)
                                              * 1000.0
                                  / static_cast<double>(d.audio_format.sample_rate))
                + "）";
        }
    }
    stat_line(s, "帧数", text(frames), nullptr,
        "每次从系统拿到多少帧（括号里是最近的最小–最大），以及采集端点的缓冲区有多大。\n"
        "块大小在 480 附近小幅波动是正常的；端点缓冲越大，延迟下限越高（括号里是毫秒）。");

    // ---- 分发：采到了 ≠ 发出去了 ----
    // 上面几行都是采集侧，回答"有没有采到"；没有客户端时 dispatcher 会整帧跳过
    // （编码 / 广播都是 0），所以"采到"不等于"客户端听得到"。这一行是主页上唯一
    // 回答"发出去没有、占多少带宽"的读数，顺带还能和音频格式里的标称码率对账：
    // 实测发送比特率应当≈标称码率。
    std::string dispatch;
    if (live) {
        dispatch = format_rate(r.broadcast_fps) + " 帧/秒 · "
            + format_rate(r.tx_pps_total()) + " 包/秒";
        const double kbps = r.tx_bytes_per_s * 8.0 / 1000.0;
        if (kbps > 0.0) {
            dispatch += (kbps >= 1000.0) ? " · " + fmt_text("%.1f Mbps", kbps / 1000.0)
                                         : " · " + fmt_text("%.0f kbps", kbps);
        }
        if (d.session.active == 0) {
            dispatch += "（无客户端）";
        }
    }
    // 有客户端却一个包都没发出去才是故障；无客户端时不发是正常状态，不标色。
    const bool not_sending = live && d.session.active > 0 && r.tx_pps_total() < 1.0;
    stat_line(s, "分发", text(dispatch), tone(not_sending, tone_error()),
        "服务端有没有真的把声音发出去：每秒广播多少帧、发出多少个包、占多少带宽。\n"
        "没有客户端时这里是 0（不发无用包，正常）；有客户端却是 0 才说明发不出去。\n"
        "最后一列可以和上面「音频格式」里的码率对一下：实测值应当差不多。");

    const auto worst = std::max(d.capture.current_starved_ms, d.capture.max_starved_ms);
    stat_line(s, "断流",
        text(fmt_text("%llu 次 · 当前 %llu ms / 最长 %llu ms",
            static_cast<unsigned long long>(d.capture.starved_events),
            static_cast<unsigned long long>(d.capture.current_starved_ms),
            static_cast<unsigned long long>(worst))),
        tone(starved, tone_warn()),
        "采集端没拿到数据的次数与时长。\n"
        "理想值是 0 次；出现次数说明采集被系统打断过（切设备、系统忙），\n"
        "这期间客户端会听到空档。当前 > 0 表示此刻还在断流。");

    // 不在主页放"欠账帧数"：它是内部记账的瞬时量（常态就是几百帧），
    // 放在这里只会让人以为出问题了。明细在 设置 → 统计。
    stat_line(s, "合成 / 事件",
        text(fmt_text("合成 %llu 块（最长 %u 轮）· 丢弃 %llu · 不连续 %llu",
            static_cast<unsigned long long>(d.capture.synthetic_silence_blocks),
            d.capture.max_synth_run_rounds,
            static_cast<unsigned long long>(d.capture.stale_events_dropped),
            static_cast<unsigned long long>(d.capture.data_discontinuity_events))),
        nullptr,
        "合成 = 没有真实数据时补进去的静音块；丢弃 = 被过滤掉的过期事件；\n"
        "不连续 = 系统告知音频流被重建的次数（切歌、切设备常见）。\n"
        "偶尔出现正常；持续增长说明采集跟不上，客户端会听到断续。");

    ImGui::EndChild();
    ImGui::PopStyleVar();
}

// ---- 会话：页面下半部分的弹性区域 ----
// 卡片吃满剩余高度：会话列表是"会长"的那一块，让它吸收窗口余量，页面底部就不会
// 剩一块没归属的空白；行多了在表内滚动，页面本身不动。
// 表格只画内部横竖分隔线（外框由卡片负责）：两张框套在一起时，8px 的间隙看起来
// 像"没对齐"，而不是"留白"。
void draw_sessions(const ServerUiState& s)
{
    const ServerSnapshot& snap = s.snapshot;
    const auto& rows = snap.sessions;

    const float pad_x = 12.0f;
    const float pad_y = 10.0f;
    // 减去 2px：卡片自己也要占边框的那一点高度，否则页面会多出一条滚动条。
    // 下限 160 = 标题行 + 表头 + **3 行数据** + 内边距（实测默认字号下：卡片顶到
    // 第一行数据 72px，行高 24px，底部留白 10px）。窗口高度不够时宁可让整页滚动，
    // 也不要把会话表压成"只剩表头"的一条缝——曾经的下限是 96（≈1 行），窗口一矮，
    // 已连接的客户端那一行就被顶出卡片，看着像"没连上"（用户 2026-11-06）。
    // 窗口够高时这张卡照旧吃满剩余高度（会话行多时在表内滚动，页面不动）。
    const float card_h = std::max(ImGui::GetContentRegionAvail().y - 2.0f, 160.0f);

    // 永远渲染：没有会话（或服务没跑）时表里给一行 "-"，不用整句话占位。
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(pad_x, pad_y));
    ImGui::BeginChild("##sessions_card", ImVec2(0.0f, card_h), ImGuiChildFlags_Borders);

    // 标题行：左「会话」，右数量。数量是"此刻有多少条"，与表里的行一一对应。
    const std::string count = snap.live ? fmt_text("%zu 个", rows.size())
                                        : std::string(kDash);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("会话");
    same_line_right(ImGui::CalcTextSize(count.c_str()).x);
    ImGui::TextDisabled("%s", count.c_str());
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("SessionManager 里存活的会话数（Created 等首包 + Connected）");
    }
    ImGui::Separator();

    // 空态只画"表头 + 一行占位"，表格按内容自适应高度，卡片剩余部分留白——
    // 满高表格的内部竖线会把整片空白切成格子，看起来像"坏掉的表"（用户实测）。
    // 有会话时才铺满并滚动。
    const bool empty = rows.empty();
    const float table_h = empty
        ? 0.0f // 0 = BeginTable 按内容自适应（表头 + 一行）
        : ImGui::GetContentRegionAvail().y;
    const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV
        | ImGuiTableFlags_BordersInnerH
        | (empty ? ImGuiTableFlags_None : ImGuiTableFlags_ScrollY)
        | ImGuiTableFlags_SizingStretchProp;
    if (ImGui::BeginTable("##sessions", 6, flags, ImVec2(0.0f, table_h))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        // 列序：会话号 → 客户端名 → 地址 → 族 → 状态 → 存活。
        // 名字紧跟在会话号后面（"这是谁"比"它连在哪"更常被问）；「最近活跃」已去掉
        // （用户 2026-11-06：没有必要显示），省下的宽度给名字列。
        ImGui::TableSetupColumn("会话", ImGuiTableColumnFlags_WidthFixed, 88.0f);
        ImGui::TableSetupColumn("客户端名", ImGuiTableColumnFlags_WidthStretch, 1.3f);
        ImGui::TableSetupColumn("地址", ImGuiTableColumnFlags_WidthStretch, 2.0f);
        ImGui::TableSetupColumn("族", ImGuiTableColumnFlags_WidthFixed, 42.0f);
        ImGui::TableSetupColumn("状态", ImGuiTableColumnFlags_WidthFixed, 78.0f);
        ImGui::TableSetupColumn("存活", ImGuiTableColumnFlags_WidthFixed, 66.0f);
        ImGui::TableHeadersRow();

        if (rows.empty()) {
            ImGui::TableNextRow();
            for (int col = 0; col < 6; ++col) {
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(kDash);
            }
        } else {
            for (const SessionRow& row : rows) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::Text("0x%08X", static_cast<unsigned>(row.id));
                ImGui::TableNextColumn();
                if (row.name.empty()) {
                    ImGui::TextDisabled("未上报");
                } else {
                    ImGui::TextUnformatted(row.name.c_str());
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("客户端在 Connect 里自报的名字（client_name，1–128 字节）\n%s",
                            row.name.c_str());
                    }
                }
                ImGui::TableNextColumn();
                if (row.endpoint.empty()) {
                    ImGui::TextDisabled("等首个 heartbeat");
                } else {
                    ImGui::TextUnformatted(row.endpoint.c_str());
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("%s（NAT 映射后的观测地址）", row.endpoint.c_str());
                    }
                }
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(row.family == 4 ? "v4" : row.family == 6 ? "v6" : "-");
                ImGui::TableNextColumn();
                if (row.connected) {
                    ImGui::TextUnformatted("已连接");
                } else {
                    ImGui::TextColored(semantic_warn(), "等首包");
                }
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(format_duration(row.age_s).c_str());
            }
        }
        ImGui::EndTable();
    }

    ImGui::EndChild();
    ImGui::PopStyleVar();
}

void draw_service_page(ServerUiState& s)
{
    // 页面整体可滚动：窗口被拖矮时不至于把最后一张表裁掉。
    ImGui::BeginChild("##service_scroll", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None);

    // 三张卡之间的缝统一成 kCardGap。为什么不能只删掉那个 Dummy：两个 item 之间
    // ImGui 会各自补一个 ItemSpacing.y，参数卡与运行信息卡之间因为多了 Dummy(4)
    // 曾经是 18px，而运行信息卡与会话卡之间只有 7px——一宽一窄（用户 2026-11-06：
    // "运行信息和上面的输入框的间隔有点大"）。这里用一次反向位移把两张卡之间的
    // 距离**精确**定到 kCardGap，与 ItemSpacing.y 的实际取值无关（密度切换后依然成立）。
    constexpr float kCardGap = 7.0f;
    const float nudge = kCardGap - ImGui::GetStyle().ItemSpacing.y;

    draw_config_card(s);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + nudge);
    draw_runtime_card(s);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + nudge);
    draw_sessions(s);

    ImGui::EndChild();
}

// ---- 设置 → 统计（core 全量诊断；滚动由设置框的 tab 容器提供）----
// 结构恒定：没在跑的时候每个值都是 "-"，不是一句话、也不是一片空白。
void draw_stats_tab(ServerUiState& s)
{
    const ServerSnapshot& snap = s.snapshot;
    const bool live = snap.live;
    const auto& d = snap.diag;
    const auto& r = snap.rates;

    const auto text = [live](std::string value) {
        return (live && !value.empty()) ? std::move(value) : std::string(kDash);
    };
    const auto tone = [live](bool condition, const ImVec4* color) {
        return (live && condition) ? color : nullptr;
    };

    ImGui::TextDisabled("每 500 ms 采一次；累计值自本次启动起算，速率是最近两次快照的差。");

    // ---- 会话与心跳 ----
    ImGui::SeparatorText("会话与心跳");
    if (begin_stat_table("##st_sess", 108.0f)) {
        stat_row(s, "存活会话", text(fmt_text("%zu 个（累计建立 %llu）", d.session.active,
                                   static_cast<unsigned long long>(d.session.created))));
        stat_row(s, "过期 / 清除", text(fmt_text("%llu / %llu",
                                       static_cast<unsigned long long>(d.session.expired),
                                       static_cast<unsigned long long>(
                                           d.session.removed_by_clear))));
        stat_row(s, "最老会话", text(d.session.session_age_ms > 0
                ? format_duration(static_cast<double>(d.session.session_age_ms) / 1000.0)
                : std::string(kDash)));
        const double timeout_s = std::max(0.001,
            std::chrono::duration<double>(s.config.session_timeout).count());
        stat_row(s, "最久未动", text(d.session.activity_count > 0
                ? fmt_text("%llu ms（超时 %.0f ms）",
                      static_cast<unsigned long long>(d.session.activity_oldest_age_ms),
                      timeout_s * 1000.0)
                : std::string(kDash)),
            tone(d.session.activity_count > 0
                    && static_cast<double>(d.session.activity_oldest_age_ms) >= timeout_s * 800.0,
                tone_warn()));
        stat_row(s, "心跳", text(fmt_text("%llu 收 / %llu 拒 / %s 每秒",
                              static_cast<unsigned long long>(d.net.heartbeat_received),
                              static_cast<unsigned long long>(d.net.heartbeat_rejected),
                              format_rate(r.heartbeat_ps).c_str())));
        ImGui::EndTable();
    }

    // ---- 采集 ----
    ImGui::SeparatorText("采集");
    if (begin_stat_table("##st_cap", 108.0f)) {
        const bool starved = (d.capture.state == aqua::audio::AudioCaptureState::Starved)
            || (d.capture.current_starved_ms > 0);
        stat_row(s, "状态", text(fmt_text("%s / %s 帧/秒", capture_state_text(d.capture.state),
                              format_rate(r.capture_fps).c_str())),
            tone(starved, tone_error()));
        stat_row(s, "回调", text(fmt_text("%llu 次（静音 %llu）",
                              static_cast<unsigned long long>(d.capture.callbacks),
                              static_cast<unsigned long long>(d.capture.silent_callbacks))));
        stat_row(s, "数据块", text(fmt_text("最近 %u / 最小 %u / 最大 %u",
                              d.capture.packet_frames_last, d.capture.packet_frames_min,
                              d.capture.packet_frames_max)));
        stat_row(s, "已采", text(fmt_text("%llu 帧 / %s",
                              static_cast<unsigned long long>(d.capture.captured_frames),
                              format_bytes(d.capture.captured_bytes).c_str())));
        const auto worst = std::max(d.capture.current_starved_ms, d.capture.max_starved_ms);
        stat_row(s, "断流", text(fmt_text("%llu 次 / 累计 %llu ms（当前 %llu / 最长 %llu）",
                              static_cast<unsigned long long>(d.capture.starved_events),
                              static_cast<unsigned long long>(d.capture.starved_ms),
                              static_cast<unsigned long long>(d.capture.current_starved_ms),
                              static_cast<unsigned long long>(worst))),
            tone(starved, tone_warn()));
        stat_row(s, "合成静音", text(fmt_text("%llu 块 / %llu 帧 / 欠账 %u 帧",
                                static_cast<unsigned long long>(
                                    d.capture.synthetic_silence_blocks),
                                static_cast<unsigned long long>(
                                    d.capture.generated_silence_frames),
                                d.capture.deficit_frames)));
        stat_row(s, "端点 / 事件", text(fmt_text("缓冲 %u 帧 / 丢弃 %llu / 不连续 %llu",
                                   d.capture.endpoint_buffer_frames,
                                   static_cast<unsigned long long>(
                                       d.capture.stale_events_dropped),
                                   static_cast<unsigned long long>(
                                       d.capture.data_discontinuity_events))));
        ImGui::EndTable();
    }

    // ---- 分帧与队列 ----
    ImGui::SeparatorText("分帧与队列");
    if (begin_stat_table("##st_queue", 108.0f)) {
        stat_row(s, "分帧", text(fmt_text("输入 %llu 块 / %s / 输出 %llu 帧",
                              static_cast<unsigned long long>(d.packetizer.input_blocks),
                              format_bytes(d.packetizer.input_bytes).c_str(),
                              static_cast<unsigned long long>(d.packetizer.frames_emitted))));
        stat_row(s, "分帧丢弃", text(fmt_text("未对齐 %llu / 待定 %llu",
                                static_cast<unsigned long long>(
                                    d.packetizer.rejected_unaligned_blocks),
                                static_cast<unsigned long long>(
                                    d.packetizer.pending_discards))));
        stat_row(s, "队列深度", text(fmt_text("%u 槽（峰值 %u / 近 200ms %u）· 积压 %s",
                                d.queue.depth_slots, d.queue.high_watermark_slots,
                                d.queue.high_watermark_slots_recent,
                                format_ms(d.queue.backlog_ms).c_str())));
        stat_row(s, "队列流量", text(fmt_text("接受 %llu / 消费 %llu / 丢弃 %llu",
                                static_cast<unsigned long long>(d.queue.accepted_frames),
                                static_cast<unsigned long long>(d.queue.consumed_frames),
                                static_cast<unsigned long long>(d.queue.dropped_frames))),
            tone(d.queue.dropped_frames > 0, tone_warn()));
        ImGui::EndTable();
    }

    // ---- 分发（编码 + 广播）----
    ImGui::SeparatorText("分发");
    if (begin_stat_table("##st_disp", 108.0f)) {
        stat_row(s, "编码 / 广播", text(fmt_text("%llu 帧（%s/秒）· 广播 %llu 帧（%s/秒）",
                                   static_cast<unsigned long long>(d.dispatcher.frames_encoded),
                                   format_rate(r.encoded_fps).c_str(),
                                   static_cast<unsigned long long>(d.dispatcher.frames_broadcast),
                                   format_rate(r.broadcast_fps).c_str())));
        stat_row(s, "无客户端", text(fmt_text("%llu 帧（%s/秒）",
                                static_cast<unsigned long long>(
                                    d.dispatcher.frames_without_clients),
                                format_rate(r.no_client_fps).c_str())));
        stat_row(s, "失败", text(fmt_text("编码 %llu / 分发 %llu / 丢弃 %llu",
                              static_cast<unsigned long long>(d.dispatcher.encode_failures),
                              static_cast<unsigned long long>(d.dispatcher.dispatch_failures),
                              static_cast<unsigned long long>(d.dispatcher.dropped_frames))),
            tone(d.dispatcher.encode_failures > 0 || d.dispatcher.dispatch_failures > 0,
                tone_warn()));
        stat_row(s, "节拍", text(fmt_text("发送 %llu / 追赶 %llu（%s）· 周期 %s",
                              static_cast<unsigned long long>(d.dispatcher.paced_sends),
                              static_cast<unsigned long long>(d.dispatcher.catchup_drains),
                              d.dispatcher.catchup_active ? "正在追赶" : "稳态",
                              format_ms(static_cast<double>(d.dispatcher.pacing_interval_ns) / 1e6)
                                  .c_str())),
            tone(d.dispatcher.catchup_active, tone_warn()));
        stat_row(s, "追赶阈值", text(fmt_text("深度 %u 槽 / 低水位 %u 槽 / 加速 %u× / 单拍 %u 包",
                                  d.dispatcher.catchup_depth_slots,
                                  d.dispatcher.catchup_low_water_slots,
                                  d.dispatcher.catchup_speedup,
                                  d.dispatcher.max_catchup_sends)));
        ImGui::EndTable();
    }

    // ---- 网络：逐族 ----
    // 未服务的族照样出表（值全是 "-"），标题上标注"未服务"——不拿一句话顶替表体。
    const auto draw_family = [&s, &text, &tone, live](const char* title, const char* table_id,
                                   const aqua::net::UdpTransportStats& t, bool served,
                                   double rx_pps, double tx_pps) {
        // 只有"服务在跑但这族没绑"才标注未服务；单纯没启动时不需要这个后缀。
        ImGui::SeparatorText((live && !served)
                ? (std::string(title) + "（未服务）").c_str()
                : title);
        if (!begin_stat_table(table_id, 96.0f)) {
            return;
        }
        stat_row(s, "收", text(fmt_text("%s 包/秒 / %s", format_rate(rx_pps).c_str(),
                              format_bytes(t.rx_bytes).c_str())));
        stat_row(s, "发", text(fmt_text("%s 包/秒 / %s", format_rate(tx_pps).c_str(),
                              format_bytes(t.tx_bytes).c_str())));
        stat_row(s, "错误", text(fmt_text("收 %llu / 发 %llu / 不可达 %llu",
                              static_cast<unsigned long long>(t.rx_errors),
                              static_cast<unsigned long long>(t.tx_errors),
                              static_cast<unsigned long long>(t.rx_unreachable))),
            tone(t.rx_errors > 0 || t.tx_errors > 0, tone_warn()));
        stat_row(s, "丢弃", text(fmt_text("发送 %llu / 入队失败 %llu",
                              static_cast<unsigned long long>(t.tx_dropped),
                              static_cast<unsigned long long>(t.tx_enqueue_failures))),
            tone(t.tx_dropped > 0, tone_warn()));
        stat_row(s, "发送队列", text(fmt_text("%zu 深度", t.tx_queue_depth)));
        stat_row(s, "接收退避", text(fmt_text("连续 %u 次 / 错误码 %d", t.rx_consecutive_errors,
                                  t.rx_last_error_code)),
            tone(t.rx_consecutive_errors > 0 || t.rx_last_error_code != 0, tone_warn()));
        ImGui::EndTable();
    };
    draw_family("数据面 IPv4", "##st_net4", d.net.transport.v4, d.net.transport.served_v4,
        r.rx_pps_v4, r.tx_pps_v4);
    draw_family("数据面 IPv6", "##st_net6", d.net.transport.v6, d.net.transport.served_v6,
        r.rx_pps_v6, r.tx_pps_v6);

    // ---- 运行时 ----
    ImGui::SeparatorText("运行时");
    if (begin_stat_table("##st_runtime", 96.0f)) {
        stat_row(s, "状态", text(std::string(aqua::runtime::runtime_state_name(d.state))));
        stat_row(s, "采集运行", text(d.capture_running ? std::string("是") : std::string("否")));
        stat_row(s, "音频错误",
            text(std::string(aqua::audio::audio_error_name(d.last_audio_error))),
            tone(d.last_audio_error != aqua::audio::AudioError::None, tone_error()));
        stat_row(s, "采集切换", text(fmt_text("%s / %s",
                                  aqua::audio::capture_switch_state_name(d.capture_switch.state),
                                  aqua::audio::switch_outcome_name(
                                      d.capture_switch.last_outcome))));
        stat_row(s, "实际设备", text(d.capture_switch.active_device_id));
        ImGui::EndTable();
    }
}

// ---- 设置 → 外观 ----
void draw_appearance_tab(ServerUiState& s)
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
                          "诊断数据是 500 ms 一拍，调高帧率只会让画面更顺滑，不会更快。");
    }
    // 实测帧率摆在右端：设置到底生效没有，一眼可见（不然只能凭"感觉顺不顺"）。
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
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("字体大小");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::SliderInt("##font_size", &s.font_size, 12, 24, "%d px");
    ImGui::TextDisabled("12 – 24 px，改动即时生效。");

    ImGui::SeparatorText("运行日志");
    // GUI 不提供 Trace（kGuiMinLogLevel）；下标 = 级别 - 下限。
    static const char* const kLevelNames[] = { "Debug", "Info", "Warn", "Error" };
    if (s.log.min_level < kGuiMinLogLevel) {
        s.log.min_level = kGuiMinLogLevel;
    }
    if (s.log.min_level > kLogError) {
        s.log.min_level = kLogError;
    }
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("级别");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120.0f);
    if (ImGui::BeginCombo("##set_log_level", kLevelNames[s.log.min_level - kGuiMinLogLevel])) {
        for (int i = kGuiMinLogLevel; i <= kLogError; ++i) {
            const bool selected = (i == s.log.min_level);
            if (ImGui::Selectable(kLevelNames[i - kGuiMinLogLevel], selected)) {
                s.log.min_level = i;
            }
            if (selected) {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine(0.0f, 24.0f);
    ImGui::Checkbox("自动滚动", &s.log.autoscroll);

    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("日志字号");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::SliderInt("##log_font_size", &s.log.font_size, 10, 20, "%d px");
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("只作用于运行日志的输出框，与上面的 UI 字号相互独立。");
    }

    ImGui::SeparatorText("其他");
    if (ImGui::Button("恢复默认", ImVec2(120.0f, 0.0f))) {
        s.theme = Theme::Dark;
        s.density = Density::Normal;
        s.rounding = 5.0f;
        s.font_size = 16;
        s.log.min_level = kLogInfo;
        s.log.autoscroll = true;
        s.log.font_size = 14;
        s.frame_rate_mode = 0;
        apply_theme(s.theme, s.dpi_scale, s.density, s.rounding);
        save_server_settings(s); // 重置也要落盘，否则下次启动又回到旧值
    }
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("改动即时生效；退出程序时保存。");
}

// ---- 设置 → 高级（音频格式 + 分帧）----
void draw_advanced_tab(ServerUiState& s)
{
    const ServerPhase phase = s.snapshot.phase;
    const bool busy = (phase == ServerPhase::Starting || phase == ServerPhase::Stopping);
    const bool editable = (phase != ServerPhase::Running) && !busy;

    ImGui::TextDisabled("改动于下次「启动服务」时生效%s。",
        editable ? "" : "；本次运行中不可修改");

    ImGui::BeginDisabled(!editable);

    ImGui::SeparatorText("音频格式");
    ImGui::Checkbox("使用采集设备的默认格式", &s.follow_format);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("推荐。设备实际格式（采样率 / 声道 / 编码）在启动后于\n"
                          "「服务」页的「音频格式」行显示。");
    }
    if (s.follow_format) {
        ImGui::TextDisabled("格式由采集设备决定；启动后显示设备实际格式。");
    } else {
        if (ImGui::BeginTable("##adv_fmt", 2, ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("k", ImGuiTableColumnFlags_WidthFixed, 84.0f);
            ImGui::TableSetupColumn("v", ImGuiTableColumnFlags_WidthStretch, 1.0f);

            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted("采样率");
            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-FLT_MIN);
            ImGui::InputInt("##adv_rate", &s.sample_rate, 100, 1000);
            s.sample_rate = std::clamp(s.sample_rate, 8000, 384000);

            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted("声道");
            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-FLT_MIN);
            ImGui::InputInt("##adv_ch", &s.channels, 0, 0);
            s.channels = std::clamp(s.channels, 1, 8);

            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted("编码");
            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::BeginCombo("##adv_enc",
                    kEncodings[std::clamp(s.encoding_index, 0, kEncodingCount - 1)].name)) {
                for (int i = 0; i < kEncodingCount; ++i) {
                    if (ImGui::Selectable(kEncodings[i].name, s.encoding_index == i)) {
                        s.encoding_index = i;
                    }
                }
                ImGui::EndCombo();
            }
            ImGui::EndTable();
        }
        ImGui::TextDisabled("设备不支持该格式时启动会失败（不会静默改成别的格式）。");
    }

    ImGui::SeparatorText("分帧");
    // 标签在左、输入框填满剩余宽度（与「采样率 / 声道」同一套），
    // 不要用 InputInt 的右侧标签——步进按钮会把它推得很远。
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("每包帧数 F");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputInt("##pkt_frames", &s.packet_frames, 1, 16);
    s.packet_frames = std::clamp(s.packet_frames, 0, 65535);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("0 = 自动（按格式推导，通常 175 @ 2ch/48kHz/s32）。\n"
                          "F 同时是包时长与客户端抖动缓冲的粒度：F 减半 = 延迟减半、包数翻倍。");
    }

    // 派生量预览：这些值在启动后由 core 决定，这里只按当前输入算一遍，
    // 好让人在改 F 之前就看到延迟 / 包速率的兑换关系。
    const int preview_rate = s.follow_format ? 48000 : s.sample_rate;
    const int preview_ch = s.follow_format ? 2 : s.channels;
    const aqua::audio::AudioEncoding preview_enc = s.follow_format
        ? aqua::audio::AudioEncoding::PCM_F32LE
        : kEncodings[std::clamp(s.encoding_index, 0, kEncodingCount - 1)].encoding;
    aqua::audio::AudioFormat preview;
    preview.encoding = preview_enc;
    preview.channels = static_cast<std::uint32_t>(std::max(1, preview_ch));
    preview.sample_rate = static_cast<std::uint32_t>(std::max(1, preview_rate));
    const std::uint32_t kbps = preview.frame_bytes() * preview.sample_rate * 8 / 1000;
    if (s.packet_frames > 0) {
        ImGui::TextDisabled("每包 %s · %s 包/秒 · %u kbps",
            format_ms(static_cast<double>(s.packet_frames) * 1000.0 / preview_rate).c_str(),
            format_rate(static_cast<double>(preview_rate) / s.packet_frames).c_str(), kbps);
    } else {
        // F = 自动时包时长 / 包速都还没定（要等 core 按格式推导），只报确定的码率。
        ImGui::TextDisabled("每包 自动（启动后按格式推导）· %u kbps", kbps);
    }

    ImGui::EndDisabled();

    if (!editable) {
        ImGui::Separator();
        ImGui::TextDisabled("运行中不可修改：这些是启动时固化进 ServerRuntime 的参数。");
    }
}

// ---- 设置窗口（模态：外观 / 统计 / 高级）----
// 与 client 的设置框同一套：居中、固定尺寸可拖大、Esc 关闭、每个 tab 自带滚动。
void draw_settings_window(ServerUiState& s)
{
    if (!s.show_settings) {
        return;
    }
    ImGui::OpenPopup(kSettingsTitle);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing,
        ImVec2(0.5f, 0.5f));
    // 首帧尺寸必须给：hidden 帧窗口是初始窄宽，靠 AutoResize 反推会把高度炸开。
    const ImVec2 work = ImGui::GetMainViewport()->WorkSize;
    const float max_h = std::max(320.0f, work.y - 40.0f);
    // 560×560：与 client 的设置框同宽（两侧并排时设置窗也齐），模态四周留出背景，
    // 不至于把整个主窗口盖死。
    ImGui::SetNextWindowSize(ImVec2(560.0f, std::min(560.0f, max_h)), ImGuiCond_Appearing);
    ImGui::SetNextWindowSizeConstraints(ImVec2(480.0f, 320.0f), ImVec2(920.0f, max_h));
    if (ImGui::BeginPopupModal(kSettingsTitle, &s.show_settings, ImGuiWindowFlags_NoCollapse)) {
        // Esc 关窗：正在文本框里打字时不抢 Esc——那一下应该先退出编辑。
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) && !ImGui::GetIO().WantTextInput) {
            s.show_settings = false;
            ImGui::CloseCurrentPopup();
        }

        const bool want_appearance = (s.settings_tab == kSettingsAppearance);
        const bool want_stats = (s.settings_tab == kSettingsStats);
        const bool want_advanced = (s.settings_tab == kSettingsAdvanced);

        struct TabSpec {
            const char* label;
            const char* child_id;
            bool wanted;
            void (*draw)(ServerUiState&);
        };
        const TabSpec tabs[] = {
            { "外观", "##tab_appearance", want_appearance, &draw_appearance_tab },
            { "统计", "##tab_stats", want_stats, &draw_stats_tab },
            { "高级", "##tab_advanced", want_advanced, &draw_advanced_tab },
        };
        if (ImGui::BeginTabBar("##settings_tabs")) {
            for (const TabSpec& tab : tabs) {
                if (ImGui::BeginTabItem(tab.label, nullptr,
                        tab.wanted ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None)) {
                    if (tab.wanted) {
                        s.settings_tab = -1; // 请求已兑现
                    }
                    // 内容区填满 tab 以下的剩余高度：内容超高时自己滚动。
                    ImGui::BeginChild(tab.child_id, ImVec2(0.0f, 0.0f));
                    tab.draw(s);
                    ImGui::EndChild();
                    ImGui::EndTabItem();
                }
            }
            ImGui::EndTabBar();
        }

        // EndPopup 必须在 if 内：BeginPopupModal 返回 false 时内部已自行 EndPopup。
        ImGui::EndPopup();
    }
}

void draw_log_page(ServerUiState& s)
{
    draw_log_panel(s.log);
}

void draw_page_tabs(ServerUiState& s)
{
    const int request = s.page_request;
    s.page_request = -1;

    if (ImGui::BeginTabBar("##srv_pages")) {
        const ImGuiTabItemFlags service_flags = (request == kPageService)
            ? ImGuiTabItemFlags_SetSelected
            : 0;
        const ImGuiTabItemFlags log_flags = (request == kPageLog)
            ? ImGuiTabItemFlags_SetSelected
            : 0;
        if (ImGui::BeginTabItem("服务", nullptr, service_flags)) {
            draw_service_page(s);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("运行日志", nullptr, log_flags)) {
            draw_log_page(s);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
}

// ---- 状态栏：阶段 / 控制端点 / 数据面实际绑定 / 运行时长 ----
// 顶部不再重复这些信息，所以这里要写全：控制面与数据面本来就分别绑定，
// "数据面到底绑在哪个地址上"是客户端连不上时的第一问（通配地址 vs 具体网卡、
// 端口是不是被系统改成了临时端口）。逐族服务与否收进 tooltip：v4+v6 这种缩写
// 放在常驻行里既占地方又看不出是哪几族。
void draw_status_bar(const ServerUiState& s)
{
    const ServerSnapshot& snap = s.snapshot;
    const bool live = (snap.phase == ServerPhase::Running);

    // 右段两件，从右到左：运行时长（贴右缘）、客户端数（在它左边）。
    // 客户端数为什么值得放状态栏：它只出现在「服务」页的会话卡里，切到「运行日志」
    // 页就完全看不见了——而"现在到底有没有人在听"是服务端最常瞄一眼的信息。
    // 服务端没有"唯一的会话号"（多客户端时每个会话一个），所以这里用数量。
    const ImGuiStyle& st = ImGui::GetStyle();
    const float gap = st.ItemSpacing.x * 2.0f;
    std::string right;
    std::string clients;
    if (live) {
        right = format_duration(snap.uptime_s);
        clients = fmt_text("%zu 个客户端", snap.sessions.size());
    }
    const float dur_w = right.empty() ? 0.0f : ImGui::CalcTextSize(right.c_str()).x;
    const float clients_w = clients.empty()
        ? 0.0f
        : ImGui::CalcTextSize(clients.c_str()).x + gap; // 含与时长之间的间隔
    const float right_total = dur_w + clients_w;

    // 左段按信息量从多到少排好候选，放不下就降级（与 client 的状态栏同一策略）：
    // 窄窗口时先丢数据面绑定，再丢控制端点——"服务在不在跑"永远看得见，
    // 完整绑定信息仍然悬停可得（tooltip 一直带 status_detail + 服务族）。
    std::string full = server_phase_label(snap.phase);
    if (!snap.rpc_endpoint.empty()) {
        full += " · 控制 " + snap.rpc_endpoint;
    }
    std::string full_data = full;
    if (live && !snap.udp_bind.empty()) {
        full_data += " · 数据 " + snap.udp_bind;
    }

    const float dot_w = ImGui::CalcTextSize("● ").x;
    const float avail = ImGui::GetContentRegionMax().x - ImGui::GetCursorPosX()
        - right_total - gap;

    std::string left;
    if (ImGui::CalcTextSize(full_data.c_str()).x <= avail - dot_w) {
        left = full_data;
    } else if (ImGui::CalcTextSize(full.c_str()).x <= avail - dot_w) {
        left = full;
    } else {
        left = server_phase_label(snap.phase);
    }

    ImGui::TextColored(phase_color(snap.phase), "● %s", left.c_str());
    std::string tip = snap.status_detail;
    if (live && !snap.served_families.empty()) {
        if (!tip.empty()) {
            tip += "\n";
        }
        tip += "数据面服务族：" + snap.served_families;
    }
    if (!tip.empty() && ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", tip.c_str());
    }
    if (!clients.empty()) {
        same_line_right(right_total);
        ImGui::TextDisabled("%s", clients.c_str());
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("本次运行中存活的客户端数（= 「服务」页会话卡里的行数）：\n"
                              "Created 等首包 + 已 Connected。");
        }
    }
    if (!right.empty()) {
        same_line_right(dur_w);
        ImGui::TextDisabled("%s", right.c_str());
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("本次运行时长（从服务启动成功算起）");
        }
    }
}

void draw_menu_bar(ServerUiState& s)
{
    if (!ImGui::BeginMainMenuBar()) {
        return;
    }
    if (ImGui::BeginMenu("文件")) {
        const ServerPhase phase = s.snapshot.phase;
        const bool running = (phase == ServerPhase::Running || phase == ServerPhase::Starting);
        const bool busy = (phase == ServerPhase::Starting || phase == ServerPhase::Stopping);
        if (ImGui::MenuItem(running ? "停止服务" : "启动服务", nullptr, false, !busy)) {
            toggle_server(s);
        }
        ImGui::Separator();
        if (ImGui::MenuItem("退出")) {
            s.quit_requested = true;
        }
        ImGui::EndMenu();
    }
    // 三个入口都打开同一个设置框，只是落在不同 tab（与 client 完全一致的路径）。
    if (ImGui::BeginMenu("设置")) {
        if (ImGui::MenuItem("外观设置…")) {
            s.settings_tab = kSettingsAppearance;
            s.show_settings = true;
        }
        if (ImGui::MenuItem("统计…")) {
            s.settings_tab = kSettingsStats;
            s.show_settings = true;
        }
        if (ImGui::MenuItem("高级设置…")) {
            s.settings_tab = kSettingsAdvanced;
            s.show_settings = true;
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("帮助")) {
        if (ImGui::MenuItem("关于 Aqua Server")) {
            s.show_about = true;
        }
        ImGui::EndMenu();
    }
    ImGui::EndMainMenuBar();
}

void draw_about_window(ServerUiState& s)
{
    if (!s.show_about) {
        return;
    }
    ImGui::OpenPopup("关于 Aqua Server");
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing,
        ImVec2(0.5f, 0.5f));
    // 与 client 的关于窗同一个坑：AlwaysAutoResize + TextWrapped 的首帧宽度很窄，
    // 高度会炸到接近视口高。给一个合理首帧宽度即可。
    ImGui::SetNextWindowSize(ImVec2(460.0f, 0.0f), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("关于 Aqua Server", &s.show_about,
            ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse
                | ImGuiWindowFlags_AlwaysAutoResize)) {
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            s.show_about = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::Text("Aqua Server");
        ImGui::TextDisabled("v%s", AQUA_SERVER_CLI_VERSION);
        ImGui::TextDisabled("局域网音频共享服务端");
        ImGui::Dummy(ImVec2(0.0f, 6.0f));

        // 与 client 的关于窗同一份清单、同一种排法（两列等宽、只列组件名）：
        // 两端是同一个仓库的产物，用的库本来就该一样。
        ImGui::SeparatorText("开源组件");
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
        ImGui::SeparatorText("项目");
        ImGui::TextLinkOpenURL("GitHub", "https://github.com/aquawius/aqua");
        ImGui::Dummy(ImVec2(0.0f, 6.0f));
        if (ImGui::Button("关闭", ImVec2(-FLT_MIN, 0.0f))) {
            s.show_about = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

} // namespace

const char* server_phase_label(ServerPhase phase)
{
    switch (phase) {
    case ServerPhase::Starting: return "启动中…";
    case ServerPhase::Running:  return "运行中";
    case ServerPhase::Stopping: return "正在停止…";
    case ServerPhase::Failed:   return "异常停止";
    case ServerPhase::Idle:
    default:                    return "已停止";
    }
}

void load_form_from_config(ServerUiState& s, const aqua::runtime::ServerRuntimeConfig& config)
{
    std::snprintf(s.rpc_ip, sizeof(s.rpc_ip), "%s", config.rpc_ip.c_str());
    std::snprintf(s.rpc_port, sizeof(s.rpc_port), "%u", static_cast<unsigned>(config.rpc_port));

    // CLI 的 "none" 在配置里就是空串：空 = 该族关闭。界面用勾选框表达，
    // 地址框保留一个可编辑的默认值（重新勾选时不用重新输入）。
    s.udp_ipv4_enabled = !config.udp_ipv4.empty();
    s.udp_ipv6_enabled = !config.udp_ipv6.empty();
    std::snprintf(s.udp_ipv4, sizeof(s.udp_ipv4), "%s",
        s.udp_ipv4_enabled ? config.udp_ipv4.c_str() : "0.0.0.0");
    std::snprintf(s.udp_ipv6, sizeof(s.udp_ipv6), "%s",
        s.udp_ipv6_enabled ? config.udp_ipv6.c_str() : "::");
    std::snprintf(s.udp_port, sizeof(s.udp_port), "%u", static_cast<unsigned>(config.udp_port));

    s.follow_format = !config.format.has_value();
    if (config.format.has_value()) {
        s.sample_rate = static_cast<int>(config.format->sample_rate);
        s.channels = static_cast<int>(config.format->channels);
        s.encoding_index = encoding_index_of(config.format->encoding);
    }
    s.packet_frames = static_cast<int>(config.frame_count);
    s.capture_source = (config.capture.source == aqua::audio::AudioCaptureSource::INPUT_DEVICE) ? 1 : 0;

    const auto& route = config.capture.route;
    if (route.is_pinned() && route.device.has_value()) {
        s.pin_device = true;
        s.pinned_device_id = route.device->value();
    }
}

void poll_capture_devices(ServerUiState& s)
{
    const ServerPhase phase = s.snapshot.phase;
    if (phase == ServerPhase::Running || phase == ServerPhase::Starting
        || phase == ServerPhase::Stopping) {
        return; // 表单禁用期间不枚举（省掉系统调用的抖动）
    }
    DeviceCache& cache = device_cache();
    const double now = ImGui::GetTime();
    if (cache.source == s.capture_source && now - cache.last_refresh < 3.0) {
        return;
    }
    cache.source = s.capture_source;
    cache.last_refresh = now;
    if (cache.manager == nullptr) {
        cache.manager = aqua::audio::create_device_manager();
    }

    s.devices.clear();
    if (cache.manager == nullptr) {
        return;
    }
    const auto direction = (s.capture_source == 1)
        ? aqua::audio::AudioDeviceDirection::INPUT
        : aqua::audio::AudioDeviceDirection::OUTPUT;
    for (const auto& device : cache.manager->enumerate(direction)) {
        s.devices.push_back(ServerUiState::DeviceItem { device.id.value(), device.name, device.is_default });
    }
}

void toggle_server(ServerUiState& s)
{
    const ServerPhase phase = s.snapshot.phase;
    if (phase == ServerPhase::Stopping) {
        return; // 收尾中：连点没有意义
    }
    if (phase == ServerPhase::Running || phase == ServerPhase::Starting) {
        s.form_error.clear();
        ServerSession::instance().stop();
        aqua::log_info("server gui: stop requested");
        return;
    }

    // 启动后表单锁定、不再枚举设备，但「当前捕获」还要显示名字：趁现在
    // （Idle）刷一次列表，否则运行中只能显示 WASAPI endpoint id。
    poll_capture_devices(s);

    aqua::runtime::ServerRuntimeConfig config;
    if (!build_config_from_form(s, config)) {
        return; // form_error 已由 build 写好
    }
    s.form_error.clear();
    s.config = config; // 记住本次实际使用的配置（重启沿用；会话超时读数也用它）
    if (!ServerSession::instance().start(config)) {
        s.form_error = "上一轮服务仍在收尾，请稍后再试";
        return;
    }
    aqua::log_info("server gui: start requested");
}

void draw_server_window(ServerUiState& s)
{
    poll_capture_devices(s);

    draw_menu_bar(s);
    setup_main_window_rect();

    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize
        | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar
        | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;
    // 菜单栏与主窗口之间不留空行（WindowPadding.y 压 0；内容区自己补一个标准间距）。
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
        ImVec2(ImGui::GetStyle().WindowPadding.x, 0.0f));
    ImGui::Begin("##aqua_server_main", nullptr, flags);
    ImGui::PopStyleVar();

    // 状态栏预留：分隔线 + 一行文字 + **底部呼吸位**。按 client 同一公式预留
    // （一个 FrameHeight + 一行行高）：实际文字用不掉这么多，剩下的就是文字与
    // 窗口底缘之间的空隙——曾经按"精确 32px"预留，结果状态栏文字贴死底边
    // （用户 2026-11-06 反馈"太靠下了，没有间隔"），宁多勿少。
    const float status_h = ImGui::GetFrameHeight() + ImGui::GetTextLineHeightWithSpacing();
    ImGui::BeginChild("##content", ImVec2(0.0f, -status_h), ImGuiChildFlags_None);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + ImGui::GetStyle().WindowPadding.y);
    draw_page_tabs(s);
    ImGui::EndChild();

    ImGui::Separator();
    draw_status_bar(s);
    ImGui::End();

    draw_settings_window(s);
    draw_about_window(s);
}

} // namespace aqua::gui
