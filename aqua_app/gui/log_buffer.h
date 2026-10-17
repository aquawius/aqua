#ifndef AQUA_APP_GUI_LOG_BUFFER_H
#define AQUA_APP_GUI_LOG_BUFFER_H

// 运行日志缓冲：线程安全环形缓冲 + 进程级单例。
// 生产者：UI 事件（现在）、core 的 spdlog sink（会话接入后）——任意线程可写。
// 消费者：UI 线程每帧快照渲染（snapshot 带级别过滤，version 供增量刷新）。
//
// 接线提示：会话层接入时不要另起炉灶，spdlog sink 直接写这里的 log_buffer()，
// 运行日志面板自动就能显示会话日志（级别沿用同一套枚举）。

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace aqua::gui {

// 日志级别（数值大可过滤，与 spdlog 的级别排序一致）。
enum LogLevel {
    kLogDebug = 0,
    kLogInfo,
    kLogWarn,
    kLogError,
};

// 一行运行日志。
struct LogLine {
    std::string stamp; // HH:MM:SS（本地时间）
    int level = kLogInfo;
    std::string text;
};

// 线程安全的日志环形缓冲（容量有界，旧行挤出）。
// Phase 0 只有 UI 事件；core 的 spdlog sink 在会话接入后写进同一缓冲。
class LogBuffer {
public:
    void append(int level, std::string text);
    void clear();
    // 按级别过滤取快照（UI 每帧最多调一次，且有增量缓存，见 ui_log.cpp）。
    [[nodiscard]] std::vector<LogLine> snapshot(int min_level) const;
    // 落盘（当前 UI 无入口；留给后续"导出日志"设置项 / 排障用）。
    [[nodiscard]] bool save_to_file(const std::string& path) const;
    // 增 / 清时自增，供 UI 做增量刷新（避免每帧拷贝全部行）。
    [[nodiscard]] std::uint64_t version() const;

private:
    static constexpr std::size_t kCapacity = 2000;
    mutable std::mutex mutex_;
    std::deque<LogLine> lines_;
    std::uint64_t version_ = 0;
};

// 进程级单例（UI 与将来的 spdlog sink 共用）。首次调用时构造，生命周期跟进程。
LogBuffer& log_buffer();

} // namespace aqua::gui

#endif // AQUA_APP_GUI_LOG_BUFFER_H
