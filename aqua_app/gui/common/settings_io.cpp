#include "settings_io.h"

#include <SDL3/SDL.h>

#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace aqua::gui {
namespace {

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

} // namespace

std::string settings_data_dir(const char* app_id)
{
    static std::string cached_id;
    static std::string cached;
    const std::string id = (app_id != nullptr && *app_id != '\0') ? app_id : "aqua";
    if (id == cached_id && !cached.empty()) {
        return cached;
    }
    if (char* pref = SDL_GetPrefPath("aquawius", id.c_str()); pref != nullptr) {
        cached = pref; // 带尾部分隔符
        SDL_free(pref);
    } else {
        // 取不到用户目录（极度精简的环境 / 沙箱）：退到 exe 同目录，至少别丢设置。
        const char* base = SDL_GetBasePath();
        cached = std::string(base != nullptr ? base : "");
    }
    cached_id = id;
    return cached;
}

std::string settings_file_path(const char* app_id)
{
    static std::string cached_id;
    static std::string cached;
    const std::string id = (app_id != nullptr && *app_id != '\0') ? app_id : "aqua";
    if (id == cached_id && !cached.empty()) {
        return cached;
    }
    const std::string dir = settings_data_dir(id.c_str());
    cached = dir.empty() ? std::string("settings.ini") : dir + "settings.ini";
    cached_id = id;
    return cached;
}

std::vector<SettingEntry> read_settings(const std::string& path)
{
    std::vector<SettingEntry> out;
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) {
        return out; // 首次启动：正常情况，不报错
    }
    const auto parse_pending = [&out](std::string& pending) {
        std::string text = trim(pending);
        pending.clear();
        while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
            text.pop_back();
        }
        text = trim(text);
        if (text.empty() || text[0] == '#') {
            return;
        }
        const std::size_t eq = text.find('=');
        if (eq == std::string::npos) {
            return;
        }
        out.push_back(SettingEntry { trim(text.substr(0, eq)), trim(text.substr(eq + 1)) });
    };
    char line[512];
    std::string pending;
    while (std::fgets(line, sizeof(line), f) != nullptr) {
        const std::size_t chunk = std::strlen(line);
        pending.append(line, chunk);
        // 一行超过 511 字节时 fgets 会分多次返回：先把余下片段续上再解析，
        // 否则后半截会被当成独立的一行，里面若带 '=' 就凭空多出一个假 key
        //（手工编辑过的 ini / 粘贴进去的长 device id 都可能触发）。
        // 判据必须是「这一片段把缓冲区填满了」：只看"末字节不是 \n"的话，
        // 末行恰好 511×N 字节且无换行时 feof 尚未置位，会一路 continue 到
        // fgets 返回 nullptr 而整行丢失；CR-only 行尾也会被整份拼成一行。
        if (chunk == sizeof(line) - 1 && line[chunk - 1] != '\n' && !std::feof(f)) {
            continue;
        }
        parse_pending(pending);
    }
    // 兜住上面那种"填满缓冲区后文件就结束"的残余：没有第二次 fgets 来触发解析。
    if (!pending.empty()) {
        parse_pending(pending);
    }
    std::fclose(f);
    return out;
}

const std::string* find_setting(const std::vector<SettingEntry>& entries, std::string_view key)
{
    for (const SettingEntry& e : entries) {
        if (std::string_view(e.key) == key) {
            return &e.value;
        }
    }
    return nullptr;
}

bool parse_setting_int(const std::string& text, long& out)
{
    if (text.empty()) {
        return false;
    }
    char* end = nullptr;
    errno = 0;
    const long value = std::strtol(text.c_str(), &end, 10);
    if (end == text.c_str() || *end != '\0') {
        return false;
    }
    // 溢出：strtol 返回 LONG_MAX/MIN 并置 errno=ERANGE。不检查的话
    // "99999999999999999999" 会被 clamp 悄悄改成一个看似合法的上限值。
    if (errno == ERANGE) {
        return false;
    }
    out = value;
    return true;
}

bool parse_setting_double(const std::string& text, double& out)
{
    if (text.empty()) {
        return false;
    }
    char* end = nullptr;
    const double value = std::strtod(text.c_str(), &end);
    if (end == text.c_str() || *end != '\0') {
        return false;
    }
    // **必须拒绝 nan/inf**：std::clamp(NaN, lo, hi) 恒返回 NaN（两个比较都 false），
    // 于是 `jb_stall_decay=nan` 会把 NaN 一路带进 JitterBuffer 配置，最后
    // static_cast<uint32_t>(NaN) 是 UB；`rounding=nan` 则让 ImGui 拿到 NaN 几何。
    // strtod 接受 "nan"/"inf"/"infinity"，所以这里显式挡掉。
    if (!std::isfinite(value)) {
        return false;
    }
    out = value;
    return true;
}

bool parse_setting_bool(const std::string& text, bool& out)
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
    if (parse_setting_int(text, value)) {
        out = (value != 0);
        return true;
    }
    return false;
}

void copy_cstr_utf8(char* dst, std::size_t cap, const std::string& src)
{
    if (cap == 0) {
        return;
    }
    // 按 **UTF-8 码点边界**截断：snprintf 的截断按字节，可能把一个三字节汉字
    // 切成"头 1~2 字节"，留下非法 UTF-8。client_name 是协议字段（服务端会原样
    // 记日志），设备名 / 主机名也可能带中文，半个码点会一路显示成 '?'。
    std::size_t n = src.size();
    if (n > cap - 1) {
        n = cap - 1;
        // 往回收：只要第 n 个字节是延续字节（10xxxxxx），说明切在码点中间。
        while (n > 0 && (static_cast<unsigned char>(src[n]) & 0xC0u) == 0x80u) {
            --n;
        }
    }
    std::memcpy(dst, src.data(), n);
    dst[n] = '\0';
}

bool write_settings_atomic(const std::string& path, const std::string& content)
{
    // 临时文件名带进程内唯一后缀：两个实例 / 两次保存不会互相删掉对方的 .tmp
    //（固定 "settings.ini.tmp" 时，A 刚写完就被 B 的 rename 抢走，A 保存失败）。
    static std::atomic<std::uint64_t> seq { 0 };
    const std::string tmp = path + ".tmp." + std::to_string(SDL_GetTicksNS())
        + "." + std::to_string(seq.fetch_add(1, std::memory_order_relaxed));
    std::FILE* f = std::fopen(tmp.c_str(), "wb");
    if (f == nullptr) {
        return false;
    }
    const bool written = std::fwrite(content.data(), 1, content.size(), f) == content.size();
    if (std::fclose(f) != 0 || !written) {
        std::remove(tmp.c_str());
        return false;
    }
    // 原子替换：SDL_RenamePath 在目标已存在时**直接替换**（SDL3 文档明确
    // "If the file at newpath already exists, it will be replaced"），且接受
    // UTF-8 路径、Windows/POSIX 同一套语义。
    // 以前是 "remove(旧文件) → std::rename"：Windows 的 CRT rename 不覆盖已存在
    // 文件，所以必须先删，但这两步之间没有原子性——进程崩在中间就是设置全丢。
    if (!SDL_RenamePath(tmp.c_str(), path.c_str())) {
        std::remove(tmp.c_str());
        return false;
    }
    return true;
}

} // namespace aqua::gui
