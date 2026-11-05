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
    // 把"通过级别过滤的行下标"写进 out（先 clear，容量保留）。一次持锁扫一遍、
    // 只比较 level，**不做任何字符串拷贝**：面板每帧只需要视口里那几十行，
    // 以前 snapshot() 却把整条缓冲（2000 行 × 4 个 std::string）全拷一遍，
    // 一次 8000 次分配，活跃会话里等于每帧一次。
    void collect_indices(int min_level, std::vector<std::uint32_t>& out) const;
    // 按下标取一行（拷贝一份给 UI；只对视口内的行调用）。下标越界返回 false
    // ——取下标与读取之间环形缓冲可能刚好挤掉了几行。
    [[nodiscard]] bool line_at(std::size_t index, LogLine& out) const;
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
    std::uint64_t version_ = 0;
};

// 进程级单例（UI 与 spdlog sink 共用）。首次调用时构造，生命周期跟进程。
LogBuffer& log_buffer();

} // namespace aqua::gui

#endif // AQUA_APP_GUI_LOG_BUFFER_H
