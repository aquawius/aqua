#include "log_buffer.h"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace aqua::gui {

// 本地时间 "YYYY-MM-DD HH:MM:SS.mmm"——与 spdlog 的 %+ 默认格式（full_formatter）
// 完全一致，面板里那一行因此与终端输出逐字符对齐。
// localtime_s / localtime_r 二选一（前者 MSVC 安全版、后者 POSIX，参数顺序不同）。
std::string log_stamp(std::chrono::system_clock::time_point tp)
{
    const std::time_t t = std::chrono::system_clock::to_time_t(tp);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        tp.time_since_epoch()).count() % 1000;
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d.%03d",
        tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
        tm.tm_hour, tm.tm_min, tm.tm_sec, static_cast<int>(ms));
    return buf;
}

const char* LogBuffer::level_name(int level)
{
    switch (level) {
    case kLogTrace: return "trace";
    case kLogDebug: return "debug";
    case kLogWarn:  return "warning";
    case kLogError: return "error";
    default:        return "info";
    }
}

void LogBuffer::append(int level, std::string text)
{
    LogLine line;
    line.stamp = log_stamp(std::chrono::system_clock::now());
    line.logger = "aqua";
    line.level = level;
    line.level_text = level_name(level);
    line.text = std::move(text);
    append_line(std::move(line));
}

void LogBuffer::append_line(LogLine line)
{
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

std::size_t LogBuffer::size() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return lines_.size();
}

void LogBuffer::collect_indices(int min_level, std::vector<std::uint32_t>& out) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    out.clear();
    out.reserve(lines_.size());
    for (std::size_t i = 0; i < lines_.size(); ++i) {
        if (lines_[i].level >= min_level) {
            out.push_back(static_cast<std::uint32_t>(i));
        }
    }
}

bool LogBuffer::line_at(std::size_t index, LogLine& out) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (index >= lines_.size()) {
        return false;
    }
    out = lines_[index];
    return true;
}

bool LogBuffer::save_to_file(const std::string& path) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (std::FILE* f = std::fopen(path.c_str(), "w"); f != nullptr) {
        for (const LogLine& line : lines_) {
            std::fprintf(f, "[%s] [%s] [%s] %s\n", line.stamp.c_str(),
                line.logger.c_str(), line.level_text.c_str(), line.text.c_str());
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
