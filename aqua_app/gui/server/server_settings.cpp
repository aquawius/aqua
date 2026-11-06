#include "server_settings.h"

#include "server_ui_state.h"
#include "settings_io.h"
#include "ui_prefs.h"

#include <aqua/runtime/server_runtime.h>

#include <algorithm>
#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace aqua::gui {
namespace {

constexpr const char* kAppId = "aqua-server";

void appendf(std::string& out, const char* fmt, ...)
{
    // 先量长度再按需要扩容：固定 256 字节缓冲会把长 device id / 主机地址悄悄
    // 截断（vsnprintf 截断时仍返回"本该写多长"，旧代码没看返回值）。
    va_list args;
    va_start(args, fmt);
    va_list measure;
    va_copy(measure, args);
    const int need = std::vsnprintf(nullptr, 0, fmt, measure);
    va_end(measure);
    if (need > 0) {
        const std::size_t at = out.size();
        out.resize(at + static_cast<std::size_t>(need) + 1);
        std::vsnprintf(out.data() + at, static_cast<std::size_t>(need) + 1, fmt, args);
        out.resize(at + static_cast<std::size_t>(need));
    }
    va_end(args);
}

} // namespace

ServerCliOverrides detect_server_cli_overrides(int argc, char** argv)
{
    ServerCliOverrides out;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = (argv[i] != nullptr)
            ? std::string_view(argv[i]) : std::string_view {};
        if (arg.size() < 3 || arg.substr(0, 2) != "--") {
            continue;
        }
        const std::size_t eq = arg.find('=');
        const std::string_view name = arg.substr(2,
            eq == std::string_view::npos ? std::string_view::npos : eq - 2);
        if (name == "rpc-ip") {
            out.rpc_ip = true;
        } else if (name == "rpc-port") {
            out.rpc_port = true;
        } else if (name == "udp-ipv4") {
            out.udp_ipv4 = true;
        } else if (name == "udp-ipv6") {
            out.udp_ipv6 = true;
        } else if (name == "udp-port") {
            out.udp_port = true;
        } else if (name == "capture") {
            out.capture_source = true;
        } else if (name == "capture-device-id") {
            out.capture_device = true;
        } else if (name == "audio-encoding" || name == "audio-channels"
            || name == "audio-sample-rate") {
            out.audio_format = true;
        } else if (name == "audio-packet-frames") {
            out.packet_frames = true;
        }
    }
    return out;
}

std::string server_data_dir()
{
    return settings_data_dir(kAppId);
}

void load_server_settings(ServerUiState& s, const aqua::runtime::ServerRuntimeConfig& cli,
    const ServerCliOverrides& overrides)
{
    const std::vector<SettingEntry> entries = read_settings(settings_file_path(kAppId));
    if (entries.empty()) {
        return;
    }
    const auto find = [&entries](const char* key) -> const std::string* {
        return find_setting(entries, key);
    };
    const auto as_int = [&find](const char* key, long& out) {
        const std::string* v = find(key);
        return v != nullptr && parse_setting_int(*v, out);
    };
    const auto as_double = [&find](const char* key, double& out) {
        const std::string* v = find(key);
        return v != nullptr && parse_setting_double(*v, out);
    };
    const auto as_bool = [&find](const char* key, bool& out) {
        const std::string* v = find(key);
        return v != nullptr && parse_setting_bool(*v, out);
    };

    if (long v = 0; as_int("theme", v)) {
        s.theme = static_cast<Theme>(std::clamp<long>(v, 0, 2));
    }
    if (long v = 0; as_int("density", v)) {
        s.density = static_cast<Density>(std::clamp<long>(v, 0, 2));
    }
    if (double v = 0.0; as_double("rounding", v)) {
        s.rounding = static_cast<float>(std::clamp(v, 0.0, 10.0));
    }
    if (long v = 0; as_int("font_size", v)) {
        s.font_size = static_cast<int>(std::clamp<long>(v, 12, 24));
    }
    if (long v = 0; as_int("frame_rate", v)) {
        s.frame_rate_mode = static_cast<int>(std::clamp<long>(v, 0, kFrameRateModeCount - 1));
    }

    // ---- 日志面板 ----
    if (long v = 0; as_int("log_level", v)) {
        // GUI 不提供 Trace：旧 ini 里的 0 也夹到 Debug。
        s.log.min_level = static_cast<int>(std::clamp<long>(v, kGuiMinLogLevel, kLogError));
    }
    as_bool("log_autoscroll", s.log.autoscroll);
    if (long v = 0; as_int("log_font_size", v)) {
        s.log.font_size = static_cast<int>(std::clamp<long>(v, 10, 20));
    }

    // ---- 服务参数表单：记住上次用的那套 ----
    // 规则（见文件头）：命令行**显式传过**的字段不碰。判据是 overrides（扫 argv
    // 得到的选项名集合），不是"值 != 默认值"——显式传默认值时那两者无法区分。
    const auto as_text = [&find](const char* key, char* dst, std::size_t cap) {
        if (const std::string* v = find(key); v != nullptr && !v->empty()) {
            copy_cstr_utf8(dst, cap, *v);
        }
    };
    if (!overrides.rpc_ip) {
        as_text("rpc_ip", s.rpc_ip, sizeof(s.rpc_ip));
    }
    if (!overrides.rpc_port) {
        as_text("rpc_port", s.rpc_port, sizeof(s.rpc_port));
    }
    if (!overrides.udp_port) {
        as_text("udp_port", s.udp_port, sizeof(s.udp_port));
    }
    if (!overrides.udp_ipv4) {
        as_text("udp_ipv4", s.udp_ipv4, sizeof(s.udp_ipv4));
        as_bool("udp_ipv4_enabled", s.udp_ipv4_enabled);
    }
    if (!overrides.udp_ipv6) {
        as_text("udp_ipv6", s.udp_ipv6, sizeof(s.udp_ipv6));
        as_bool("udp_ipv6_enabled", s.udp_ipv6_enabled);
    }
    if (!overrides.capture_source) {
        if (long v = 0; as_int("capture_source", v)) {
            s.capture_source = (v == 1) ? 1 : 0;
        }
    }
    if (!overrides.capture_device && !cli.capture.route.is_pinned()) {
        as_bool("pin_device", s.pin_device);
        if (const std::string* v = find("pinned_device_id"); v != nullptr && !v->empty()) {
            s.pinned_device_id = *v;
        }
    }
    if (!overrides.audio_format && !cli.format.has_value()) {
        as_bool("follow_format", s.follow_format);
        if (!s.follow_format) {
            if (long v = 0; as_int("sample_rate", v)) {
                s.sample_rate = static_cast<int>(std::clamp<long>(v, 8000, 384000));
            }
            if (long v = 0; as_int("channels", v)) {
                s.channels = static_cast<int>(std::clamp<long>(v, 1, 8));
            }
            // 0–4 = ui_server 里 kEncodings 的项数；越界值交给 encoding_index_of 兜底。
            if (long v = 0; as_int("encoding_index", v)) {
                s.encoding_index = static_cast<int>(std::clamp<long>(v, 0, 4));
            }
        }
    }
    if (!overrides.packet_frames) {
        if (long v = 0; as_int("packet_frames", v)) {
            s.packet_frames = static_cast<int>(std::clamp<long>(v, 0, 65535));
        }
    }
}

bool save_server_settings(const ServerUiState& s)
{
    std::string text;
    text.reserve(512);
    appendf(text, "# aqua_server_gui settings (auto-generated; safe to edit by hand)\n");
    appendf(text, "theme=%d\n", static_cast<int>(s.theme));
    appendf(text, "density=%d\n", static_cast<int>(s.density));
    appendf(text, "rounding=%.1f\n", static_cast<double>(s.rounding));
    appendf(text, "font_size=%d\n", s.font_size);
    appendf(text, "frame_rate=%d\n", s.frame_rate_mode);
    appendf(text, "log_level=%d\n", s.log.min_level);
    appendf(text, "log_autoscroll=%d\n", s.log.autoscroll ? 1 : 0);
    appendf(text, "log_font_size=%d\n", s.log.font_size);

    // ---- 服务参数表单（"上次的表单"；优先级见文件头）----
    appendf(text, "rpc_ip=%s\n", s.rpc_ip);
    appendf(text, "rpc_port=%s\n", s.rpc_port);
    appendf(text, "udp_ipv4=%s\n", s.udp_ipv4);
    appendf(text, "udp_ipv6=%s\n", s.udp_ipv6);
    appendf(text, "udp_ipv4_enabled=%d\n", s.udp_ipv4_enabled ? 1 : 0);
    appendf(text, "udp_ipv6_enabled=%d\n", s.udp_ipv6_enabled ? 1 : 0);
    appendf(text, "udp_port=%s\n", s.udp_port);
    appendf(text, "capture_source=%d\n", s.capture_source);
    appendf(text, "pin_device=%d\n", s.pin_device ? 1 : 0);
    // 设备 id 里可以有逗号 / 等号 / 中文（WASAPI 是 GUID，AAudio 是十六进制串，
    // 稳妥起见不假设字符集）——本格式是整行 key=value，只要值里不换行就安全。
    appendf(text, "pinned_device_id=%s\n", s.pinned_device_id.c_str());
    appendf(text, "follow_format=%d\n", s.follow_format ? 1 : 0);
    if (!s.follow_format) {
        appendf(text, "sample_rate=%d\n", s.sample_rate);
        appendf(text, "channels=%d\n", s.channels);
        appendf(text, "encoding_index=%d\n", s.encoding_index);
    }
    appendf(text, "packet_frames=%d\n", s.packet_frames);

    return write_settings_atomic(settings_file_path(kAppId), text);
}

} // namespace aqua::gui
