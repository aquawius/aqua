#include "app_settings.h"

#include "jb_presets.h"
#include "log_buffer.h"
#include "ui_state.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace aqua::gui {
namespace {

struct Entry {
    std::string key;
    std::string value;
};

std::string trim(std::string text)
{
    const auto is_space = [](char c) { return c == ' ' || c == '\t'; };
    std::size_t begin = 0;
    while (begin < text.size() && is_space(text[begin])) {
        ++begin;
    }
    std::size_t end = text.size();
    while (end > begin && is_space(text[end - 1])) {
        --end;
    }
    return text.substr(begin, end - begin);
}

// 逐行读 key=value。坏行（没有 '=' / 空行 / 注释）直接跳过。
std::vector<Entry> read_entries(const std::string& path)
{
    std::vector<Entry> out;
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) {
        return out;
    }
    char line[512];
    while (std::fgets(line, sizeof(line), f) != nullptr) {
        std::string text = line;
        while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
            text.pop_back();
        }
        text = trim(text);
        if (text.empty() || text[0] == '#') {
            continue;
        }
        const std::size_t eq = text.find('=');
        if (eq == std::string::npos) {
            continue;
        }
        out.push_back(Entry { trim(text.substr(0, eq)), trim(text.substr(eq + 1)) });
    }
    std::fclose(f);
    return out;
}

bool parse_int(const std::string& text, long& out)
{
    if (text.empty()) {
        return false;
    }
    char* end = nullptr;
    const long value = std::strtol(text.c_str(), &end, 10);
    if (end == text.c_str() || *end != '\0') {
        return false;
    }
    out = value;
    return true;
}

bool parse_double(const std::string& text, double& out)
{
    if (text.empty()) {
        return false;
    }
    char* end = nullptr;
    const double value = std::strtod(text.c_str(), &end);
    if (end == text.c_str() || *end != '\0') {
        return false;
    }
    out = value;
    return true;
}

// 布尔写成 1/0，同时接受 true/false / on/off（手改文件时不至于踩空）。
bool parse_bool(const std::string& text, bool& out)
{
    if (text == "1" || text == "true" || text == "on") {
        out = true;
        return true;
    }
    if (text == "0" || text == "false" || text == "off") {
        out = false;
        return true;
    }
    long value = 0;
    if (parse_int(text, value)) {
        out = (value != 0);
        return true;
    }
    return false;
}

void copy_cstr(char* dst, std::size_t cap, const std::string& src)
{
    if (cap == 0) {
        return;
    }
    std::snprintf(dst, cap, "%s", src.c_str());
}

} // namespace

std::string data_dir()
{
    // SDL_GetPrefPath 每次调用都会分配内存并确保目录存在，结果缓存一份即可。
    static std::string cached;
    if (!cached.empty()) {
        return cached;
    }
    if (char* pref = SDL_GetPrefPath("aquawius", "aqua-client"); pref != nullptr) {
        cached = pref; // 带尾部分隔符
        SDL_free(pref);
    } else {
        // 取不到用户目录（极度精简的环境 / 沙箱）：退到 exe 同目录，至少别丢设置。
        const char* base = SDL_GetBasePath();
        cached = std::string(base != nullptr ? base : "");
    }
    return cached;
}

std::string settings_path()
{
    static std::string cached;
    if (cached.empty()) {
        const std::string dir = data_dir();
        cached = dir.empty() ? std::string("settings.ini") : dir + "settings.ini";
    }
    return cached;
}

void load_settings(UiState& s)
{
    const std::vector<Entry> entries = read_entries(settings_path());
    if (entries.empty()) {
        return; // 首次启动 / 空文件：全用默认值
    }
    const auto find = [&entries](const char* key) -> const std::string* {
        for (const Entry& e : entries) {
            if (e.key == key) {
                return &e.value;
            }
        }
        return nullptr;
    };
    const auto as_int = [&find](const char* key, long& out) {
        const std::string* v = find(key);
        return v != nullptr && parse_int(*v, out);
    };
    const auto as_double = [&find](const char* key, double& out) {
        const std::string* v = find(key);
        return v != nullptr && parse_double(*v, out);
    };
    const auto as_bool = [&find](const char* key, bool& out) {
        const std::string* v = find(key);
        return v != nullptr && parse_bool(*v, out);
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
        copy_cstr(s.rpc_ip, sizeof(s.rpc_ip), *v);
    }
    if (const std::string* v = find("rpc_port")) {
        copy_cstr(s.rpc_port, sizeof(s.rpc_port), *v);
    }
    if (const std::string* v = find("udp_port")) {
        copy_cstr(s.udp_port, sizeof(s.udp_port), *v);
    }
    if (const std::string* v = find("client_name")) {
        copy_cstr(s.client_name, sizeof(s.client_name), *v);
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
        s.log_min_level = static_cast<int>(std::clamp<long>(v, kLogTrace, kLogError));
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
    const std::string path = settings_path();
    const std::string tmp = path + ".tmp";
    std::FILE* f = std::fopen(tmp.c_str(), "wb");
    if (f == nullptr) {
        log_buffer().append(kLogWarn, "failed to save settings: cannot write " + path);
        return false;
    }

    std::fprintf(f, "# aqua_client_gui settings (auto-generated; safe to edit by hand)\n");
    std::fprintf(f, "theme=%d\n", static_cast<int>(s.theme));
    std::fprintf(f, "density=%d\n", static_cast<int>(s.density));
    std::fprintf(f, "rounding=%.1f\n", static_cast<double>(s.rounding));
    std::fprintf(f, "font_size=%d\n", s.font_size);
    std::fprintf(f, "frame_rate=%d\n", s.frame_rate_mode);

    std::fprintf(f, "rpc_ip=%s\n", s.rpc_ip);
    std::fprintf(f, "rpc_port=%s\n", s.rpc_port);
    std::fprintf(f, "udp_port=%s\n", s.udp_port);
    std::fprintf(f, "client_name=%s\n", s.client_name);
    std::fprintf(f, "preset=%d\n", s.preset_index);
    std::fprintf(f, "custom_config=%d\n", s.custom_config ? 1 : 0);
    std::fprintf(f, "auto_reconnect=%d\n", s.auto_reconnect ? 1 : 0);
    std::fprintf(f, "reconnect_delay=%d\n", s.reconnect_delay_s);

    constexpr const char* kKeys[kJbParamCount] = {
        "jb_capacity", "jb_min_target", "jb_stall_peak_cap",
        "jb_stall_decay", "jb_stall_threshold", "jb_underrun_penalty",
    };
    for (int i = 0; i < kJbParamCount; ++i) {
        std::fprintf(f, "%s=%g\n", kKeys[i], jb_param_get(s.jb_custom, i));
    }
    std::fprintf(f, "jb_adaptive=%d\n", s.jb_adaptive_target ? 1 : 0);
    std::fprintf(f, "jb_conceal=%d\n", s.jb_concealment ? 1 : 0);
    std::fprintf(f, "jb_splice=%d\n", s.jb_splice ? 1 : 0);
    std::fprintf(f, "jb_trace=%d\n", s.jb_packet_trace ? 1 : 0);
    std::fprintf(f, "playback_buffer_frames=%d\n", s.playback_buffer_frames);

    std::fprintf(f, "log_level=%d\n", s.log_min_level);
    std::fprintf(f, "log_autoscroll=%d\n", s.log_autoscroll ? 1 : 0);
    std::fprintf(f, "log_font_size=%d\n", s.log_font_size);
    std::fprintf(f, "log_capacity=%d\n", s.log_capacity);

    if (std::fclose(f) != 0) {
        std::remove(tmp.c_str());
        log_buffer().append(kLogWarn, "failed to save settings: write interrupted " + tmp);
        return false;
    }
    // 原子替换：Windows 的 rename 不会覆盖已存在文件，先删旧文件。
    std::remove(path.c_str());
    if (std::rename(tmp.c_str(), path.c_str()) != 0) {
        std::remove(tmp.c_str());
        log_buffer().append(kLogWarn, "failed to save settings: cannot replace " + path);
        return false;
    }
    return true;
}

} // namespace aqua::gui
