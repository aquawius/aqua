#ifndef AQUA_APP_GUI_LOG_BUFFER_H
#define AQUA_APP_GUI_LOG_BUFFER_H

// 运行日志缓冲：线程安全环形缓冲 + 进程级单例。
// 生产者：core 的 spdlog sink（主）、UI 事件（少量）——任意线程可写。
// 消费者：UI 线程每帧快照渲染（snapshot 带级别过滤，version 供增量刷新）。
//
// 设计目标：面板里那一行要和终端里看到的 spdlog 输出**长得一模一样**
// —— `[2026-10-18 10:42:57.123] [aqua] [info] message`，级别带颜色。
// 所以这里存的是结构化字段（时间戳 / logger 名 / spdlog 级别原文 / 正文），
// 而不是一句拼好的中文文本；拼装与上色由面板负责。

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace aqua::gui {

// 本地时间 "YYYY-MM-DD HH:MM:SS.mmm"（与 spdlog 的 %+ 默认格式逐字符一致）。
// 抽成自由函数是因为 spdlog sink 要用 log_msg.time 而不是接收时刻，两处共用
// 同一套格式化，面板里的时间戳才能和终端一字不差。
std::string log_stamp(std::chrono::system_clock::time_point tp);

// 过滤用的 5 档级别（数值大 = 更严重，与 spdlog 的级别排序一致）。
// 2026-10-18 加回 trace 独立档：设置 → 高级的「逐包 trace」开关打开后，core 每收
// 一个音频包打一行 JBT …，没有 Trace 档这些行在面板里根本看不到（开关等于白设）。
// critical 归到 kLogError——过滤粒度不需要 6 档；但**显示**用的是
// LogLine::level_text（spdlog 原文：trace/debug/info/warning/error/critical），
// 过滤合并归合并，信息不丢。
enum LogLevel {
    kLogTrace = 0,
    kLogDebug,
    kLogInfo,
    kLogWarn,
    kLogError,
};

// 一行运行日志（字段与 spdlog 的 log_msg 对应）。
struct LogLine {
    std::string stamp;      // 本地时间 "2026-10-18 10:42:57.123"
    std::string logger;     // logger 名（core 的是 "aqua"）
    int level = kLogInfo;   // 过滤用
    std::string level_text; // 显示用：trace / debug / info / warning / error / critical
    std::string text;       // 正文
};

// 线程安全的日志环形缓冲（容量有界，旧行挤出）。
class LogBuffer {
public:
    // UI 侧快捷写法：时间戳取当前时刻、logger 记 "aqua"、级别名按 5 档映射。
    void append(int level, std::string text);
    // spdlog sink / 演示数据用：字段自带（时间戳来自 log_msg.time，不取接收时刻）。
    void append_line(LogLine line);
    void clear();
    // 按级别过滤取快照（UI 每帧最多调一次，且有增量缓存，见 ui_log.cpp）。
    [[nodiscard]] std::vector<LogLine> snapshot(int min_level) const;
    // 落盘（当前 UI 无入口；留给后续"导出日志"设置项 / 排障用）。
    [[nodiscard]] bool save_to_file(const std::string& path) const;
    // 增 / 清时自增，供 UI 做增量刷新（避免每帧拷贝全部行）。
    [[nodiscard]] std::uint64_t version() const;

    // 5 档级别 → spdlog 的级别名（append 用；sink 自己带原文）。
    [[nodiscard]] static const char* level_name(int level);

private:
    static constexpr std::size_t kCapacity = 2000;
    mutable std::mutex mutex_;
    std::deque<LogLine> lines_;
    std::uint64_t version_ = 0;
};

// 进程级单例（UI 与 spdlog sink 共用）。首次调用时构造，生命周期跟进程。
LogBuffer& log_buffer();

} // namespace aqua::gui

#endif // AQUA_APP_GUI_LOG_BUFFER_H
