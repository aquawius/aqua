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
// 2026-10-18 加回 trace 独立档；2026-11-07 起 **GUI 不再提供**（最低 kLogDebug）：
// Trace 下每秒数千行，几秒就能顶满 2000 行缓冲，Debug 构建的日志面板也会被拖慢。
// 逐包 trace 仍保留给 CLI（--log-level trace）；kLogTrace 继续用于 sink 映射。
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

// GUI 端允许的最低日志级别：下拉框、旧 settings.ini、命令行入口都夹到这一档。
// CLI 程序不受影响（仍可 --log-level trace）。
constexpr int kGuiMinLogLevel = kLogDebug;

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
    // UI 侧快捷写法：时间戳取当前时刻、logger 记 "aqua_gui"、级别名按 5 档映射。
    void append(int level, std::string text);
    // spdlog sink / 演示数据用：字段自带（时间戳来自 log_msg.time，不取接收时刻）。
    void append_line(LogLine line);
    void clear();
    // 行数上限（设置 → 外观 → 运行日志可调）。缩小会当场裁掉最旧的行，并自增
    // version —— 面板的缓存下标因此失效重取，不会指向错误的行。
    void set_capacity(std::size_t lines);
    [[nodiscard]] std::size_t capacity() const;
    // 行数（含被级别过滤掉的）——行下标空间的上界。
    [[nodiscard]] std::size_t size() const;
    // 把"通过级别过滤的行的**稳定序号**"写进 out（先 clear，容量保留）。一次持锁
    // 扫一遍、只比较 level，**不做任何字符串拷贝**：面板每帧只需要视口里那几十行，
    // 以前 snapshot() 却把整条缓冲（2000 行 × 4 个 std::string）全拷一遍，
    // 一次 8000 次分配，活跃会话里等于每帧一次。
    //
    // ⚠️ 存的是序号，不是 deque 下标：collect_indices 与 line_at 分两次加锁，
    // 中间只要 append 触发了 pop_front()，所有下标就左移一位——面板会显示错行
    //（末尾还会取不到而少一行），直到下一帧 version 变化才纠正。序号单调不复用，
    // 环形滚动后仍能指向同一行（或明确返回 false 表示已被挤出）。
    void collect_indices(int min_level, std::vector<std::uint64_t>& out) const;
    // 按稳定序号取一行（拷贝一份给 UI；只对视口内的行调用）。序号已被挤出或
    // 缓冲被清空时返回 false。
    [[nodiscard]] bool line_at(std::uint64_t sequence, LogLine& out) const;
    // 落盘（当前 UI 无入口；留给后续"导出日志"设置项 / 排障用）。
    [[nodiscard]] bool save_to_file(const std::string& path) const;
    // 增 / 清时自增，供 UI 做增量刷新（避免每帧拷贝全部行）。
    [[nodiscard]] std::uint64_t version() const;

    // 5 档级别 → spdlog 的级别名（append 用；sink 自己带原文）。
    [[nodiscard]] static const char* level_name(int level);

private:
    static constexpr std::size_t kCapacity = 2000;    // 默认行数上限
    static constexpr std::size_t kMinCapacity = 200;  // 再小就看不出上下文
    static constexpr std::size_t kMaxCapacity = 10000; // 再大就是白占内存（每行 ~200B）
    mutable std::mutex mutex_;
    std::deque<LogLine> lines_;
    std::size_t capacity_ = kCapacity;
    // lines_[0] 的全局序号，以及下一行将拿到的序号。两者之差恒等于 lines_.size()。
    std::uint64_t first_sequence_ = 0;
    std::uint64_t next_sequence_ = 0;
    std::uint64_t version_ = 0;
};

// 进程级单例（UI 与 spdlog sink 共用）。首次调用时构造，生命周期跟进程。
LogBuffer& log_buffer();

} // namespace aqua::gui

#endif // AQUA_APP_GUI_LOG_BUFFER_H
