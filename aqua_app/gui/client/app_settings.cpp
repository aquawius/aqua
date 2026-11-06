#include "app_settings.h"

#include "jb_presets.h"
#include "log_buffer.h"
#include "settings_io.h"
#include "ui_state.h"

#include <algorithm>
#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <string>

// 客户端偏好持久化：只保存**用户偏好**（主题 / 字体 / 刷新率 / 连接参数 / JB
// 参数 / 日志视图），不保存运行期状态（会话、指标、日志缓冲、窗口开关）。
//
// 通用的 key=value 读写与目录规则在 common/settings_io.*（server 端共用同一份）；
// 本文件只负责"UiState ↔ 键值表"的映射与范围钳制。

namespace aqua::gui {
namespace {

void appendf(std::string& out, const char* fmt, ...)
{
    // 先量长度再按需扩容：固定 256 字节缓冲会静默截断长值（client_name / 设备 id
    // 都在这个文件里写），截断时行尾的 \n 也会一起丢掉，下一行就被粘上来。
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

std::string data_dir()
{
    return settings_data_dir("aqua-client");
}

std::string settings_path()
{
    return settings_file_path("aqua-client");
}

void load_settings(UiState& s)
{
    const std::vector<SettingEntry> entries = read_settings(settings_path());
    if (entries.empty()) {
        return; // 首次启动 / 空文件：全用默认值
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

    // ---- 外观 ----
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

    // ---- 连接参数 ----
    // 设备选择刻意不持久化：WASAPI / AAudio 的 endpoint id 跨进程不稳定，
    // 存下标下次指向另一台设备，比"不记住"更糟。
    if (const std::string* v = find("rpc_ip")) {
        copy_cstr_utf8(s.rpc_ip, sizeof(s.rpc_ip), *v);
    }
    if (const std::string* v = find("rpc_port")) {
        copy_cstr_utf8(s.rpc_port, sizeof(s.rpc_port), *v);
    }
    if (const std::string* v = find("udp_port")) {
        copy_cstr_utf8(s.udp_port, sizeof(s.udp_port), *v);
    }
    if (const std::string* v = find("client_name")) {
        copy_cstr_utf8(s.client_name, sizeof(s.client_name), *v);
    }
    if (long v = 0; as_int("preset", v)) {
        s.preset_index = static_cast<int>(std::clamp<long>(v, 0, kJbPresetCount - 1));
    }
    as_bool("custom_config", s.custom_config);
    as_bool("auto_reconnect", s.auto_reconnect);
    if (long v = 0; as_int("reconnect_delay", v)) {
        s.reconnect_delay_s = static_cast<int>(std::clamp<long>(v, 1, 10));
    }

    // ---- JB 参数（自定义预设参数用；0 = core 默认，范围取与滑条同一张规格表）----
    {
        const JbParamSpec (&specs)[kJbParamCount] = jb_param_specs();
        constexpr const char* kKeys[kJbParamCount] = {
            "jb_capacity", "jb_min_target", "jb_stall_peak_cap",
            "jb_stall_decay", "jb_stall_threshold", "jb_underrun_penalty",
        };
        for (int i = 0; i < kJbParamCount; ++i) {
            if (double v = 0.0; as_double(kKeys[i], v)) {
                jb_param_set(s.jb_custom, i, std::clamp(v, specs[i].min, specs[i].max));
            }
        }
    }
    as_bool("jb_adaptive", s.jb_adaptive_target);
    as_bool("jb_conceal", s.jb_concealment);
    as_bool("jb_splice", s.jb_splice);
    as_bool("jb_trace", s.jb_packet_trace);
    if (long v = 0; as_int("playback_buffer_frames", v)) {
        s.playback_buffer_frames = static_cast<int>(std::clamp<long>(v, 0, 8192));
    }

    // ---- 日志视图 ----
    if (long v = 0; as_int("log_level", v)) {
        // GUI 不提供 Trace：旧 ini 里的 0 也夹到 Debug。
        s.log_min_level = static_cast<int>(std::clamp<long>(v, kGuiMinLogLevel, kLogError));
    }
    as_bool("log_autoscroll", s.log_autoscroll);
    if (long v = 0; as_int("log_font_size", v)) {
        s.log_font_size = static_cast<int>(std::clamp<long>(v, 10, 20));
    }
    if (long v = 0; as_int("log_capacity", v)) {
        s.log_capacity = static_cast<int>(std::clamp<long>(v, 200, 10000));
    }
}

bool save_settings(const UiState& s)
{
    std::string text;
    text.reserve(1024);
    appendf(text, "# aqua_client_gui settings (auto-generated; safe to edit by hand)\n");
    appendf(text, "theme=%d\n", static_cast<int>(s.theme));
    appendf(text, "density=%d\n", static_cast<int>(s.density));
    appendf(text, "rounding=%.1f\n", static_cast<double>(s.rounding));
    appendf(text, "font_size=%d\n", s.font_size);
    appendf(text, "frame_rate=%d\n", s.frame_rate_mode);

    appendf(text, "rpc_ip=%s\n", s.rpc_ip);
    appendf(text, "rpc_port=%s\n", s.rpc_port);
    appendf(text, "udp_port=%s\n", s.udp_port);
    appendf(text, "client_name=%s\n", s.client_name);
    appendf(text, "preset=%d\n", s.preset_index);
    appendf(text, "custom_config=%d\n", s.custom_config ? 1 : 0);
    appendf(text, "auto_reconnect=%d\n", s.auto_reconnect ? 1 : 0);
    appendf(text, "reconnect_delay=%d\n", s.reconnect_delay_s);

    constexpr const char* kKeys[kJbParamCount] = {
        "jb_capacity", "jb_min_target", "jb_stall_peak_cap",
        "jb_stall_decay", "jb_stall_threshold", "jb_underrun_penalty",
    };
    for (int i = 0; i < kJbParamCount; ++i) {
        appendf(text, "%s=%g\n", kKeys[i], jb_param_get(s.jb_custom, i));
    }
    appendf(text, "jb_adaptive=%d\n", s.jb_adaptive_target ? 1 : 0);
    appendf(text, "jb_conceal=%d\n", s.jb_concealment ? 1 : 0);
    appendf(text, "jb_splice=%d\n", s.jb_splice ? 1 : 0);
    appendf(text, "jb_trace=%d\n", s.jb_packet_trace ? 1 : 0);
    appendf(text, "playback_buffer_frames=%d\n", s.playback_buffer_frames);

    appendf(text, "log_level=%d\n", s.log_min_level);
    appendf(text, "log_autoscroll=%d\n", s.log_autoscroll ? 1 : 0);
    appendf(text, "log_font_size=%d\n", s.log_font_size);
    appendf(text, "log_capacity=%d\n", s.log_capacity);

    const std::string path = settings_path();
    if (!write_settings_atomic(path, text)) {
        log_buffer().append(kLogWarn, "failed to save settings: cannot write " + path);
        return false;
    }
    return true;
}

} // namespace aqua::gui
