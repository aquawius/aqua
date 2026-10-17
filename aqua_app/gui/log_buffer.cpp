#include "log_buffer.h"

#include <cstdio>
#include <ctime>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace aqua::gui {
namespace {

// 本地时间 HH:MM:SS。localtime_s / localtime_r 二选一（前者 MSVC 安全版、
// 后者 POSIX；参数顺序也不同，别写混）。
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

} // namespace

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

} // namespace aqua::gui
