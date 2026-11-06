#include "ui_format.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>

namespace aqua::gui {
namespace {

// 统一走 snprintf 再转 string：与仓库里既有的显示代码同一条路径（locale 无关）。
std::string format_via(const char* fmt, ...)
{
    char buf[64];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    return buf;
}

} // namespace

float mono_size_for(int ui_font_size)
{
    return static_cast<float>(ui_font_size) * kMonoRatio;
}

float mono_baseline_dy_for(ImFont* mono, float mono_px)
{
    if (mono == nullptr) {
        return 0.0f;
    }
    // 先量等宽字体的 ascent（push 一下即可，baked 数据是惰性烘焙 + 缓存的）。
    ImGui::PushFont(mono, mono_px);
    const float mono_ascent = ImGui::GetFontBaked()->Ascent;
    ImGui::PopFont();
    const float label_ascent = ImGui::GetFontBaked()->Ascent;
    const float dy = label_ascent - mono_ascent;
    return (dy > 0.0f) ? dy : 0.0f;
}

std::string format_bytes(std::uint64_t bytes)
{
    if (bytes < 1024) {
        return format_via("%llu B", static_cast<unsigned long long>(bytes));
    }
    if (bytes < 1024 * 1024) {
        return format_via("%.1f KB", static_cast<double>(bytes) / 1024.0);
    }
    return format_via("%.2f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
}

std::string format_duration(double seconds)
{
    // NaN/inf 不能进 static_cast<int>（后者是 UB）。uptime 正常不会是这两个值，
    // 但本函数也被统计页拿来格式化"最老会话年龄"这类来自诊断的值。
    if (!std::isfinite(seconds) || seconds <= 0.0) {
        return "00:00";
    }
    // 上限一年：再大也不该溢出 int（"%d" 是 32 位小时数）。
    const int total = static_cast<int>(std::min(seconds, 86400.0 * 365.0));
    if (total < 3600) {
        return format_via("%02d:%02d", total / 60, total % 60);
    }
    return format_via("%d:%02d:%02d", total / 3600, (total / 60) % 60, total % 60);
}

std::string format_sample_rate(int hz)
{
    if (hz <= 0) {
        return "-";
    }
    if (hz % 1000 == 0) {
        return format_via("%d kHz", hz / 1000);
    }
    return format_via("%.1f kHz", static_cast<double>(hz) / 1000.0);
}

std::string format_channels(int channels)
{
    switch (channels) {
    case 0: return "-";
    case 1: return "单声道";
    case 2: return "立体声";
    default: return format_via("%d 声道", channels);
    }
}

std::string format_ms(double ms)
{
    if (!std::isfinite(ms)) {
        return "-";
    }
    if (std::fabs(ms) < 10.0) {
        return format_via("%.2f ms", ms);
    }
    return format_via("%.1f ms", ms);
}

std::string format_rate(double per_second)
{
    if (!std::isfinite(per_second) || per_second < 0.0) {
        return "-";
    }
    if (per_second >= 10000.0) {
        return format_via("%.1fk", per_second / 1000.0);
    }
    if (per_second >= 1000.0) {
        return format_via("%.2fk", per_second / 1000.0);
    }
    if (per_second >= 100.0) {
        return format_via("%.0f", per_second);
    }
    return format_via("%.1f", per_second);
}

} // namespace aqua::gui
