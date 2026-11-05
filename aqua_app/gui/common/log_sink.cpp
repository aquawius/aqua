#include "log_sink.h"

#include "log_buffer.h"

#include <spdlog/sinks/base_sink.h>
#include <spdlog/spdlog.h>

#include <mutex>
#include <string>
#include <utility>

namespace aqua::gui {
namespace {

// spdlog 级别 → GUI 的 5 档过滤级别（数值大 = 更严重，两侧排序一致）。
// trace 单独一档（设置 → 高级的「逐包 trace」开关要能看到 JBT 行）；
// critical 并入 error：过滤粒度不需要 6 档，显示仍用 spdlog 原文。
int map_level(spdlog::level::level_enum level)
{
    switch (level) {
    case spdlog::level::trace:
        return kLogTrace;
    case spdlog::level::debug:
        return kLogDebug;
    case spdlog::level::info:
        return kLogInfo;
    case spdlog::level::warn:
        return kLogWarn;
    case spdlog::level::err:
    case spdlog::level::critical:
    case spdlog::level::off:
    default:
        return kLogError;
    }
}

// base_sink<mutex> 负责在 sink_it_ 外层加锁（与 LogBuffer 自己的锁叠加，无害：
// 前者保护 sink 内部状态，后者保护环形缓冲，两个层面各自自洽）。
class GuiLogSink final : public spdlog::sinks::base_sink<std::mutex> {
protected:
    void sink_it_(const spdlog::details::log_msg& msg) override
    {
        LogLine line;
        // 时间戳用 log_msg 自带的（不是"UI 收到的时刻"）：终端那行显示的就是它，
        // 两者必须一致，否则排障时对不上。
        line.stamp = log_stamp(msg.time);
        line.logger = std::string(msg.logger_name.data(), msg.logger_name.size());
        line.level = map_level(msg.level);
        const spdlog::string_view_t level_name = spdlog::level::to_string_view(msg.level);
        line.level_text = std::string(level_name.data(), level_name.size());
        line.text = std::string(msg.payload.data(), msg.payload.size());
        log_buffer().append_line(std::move(line));
    }

    void flush_() override
    {
        // LogBuffer 是内存环形缓冲，没有可 flush 的落盘状态。
    }
};

} // namespace

void install_gui_log_sink()
{
    if (auto logger = spdlog::default_logger(); logger != nullptr) {
        logger->sinks().push_back(std::make_shared<GuiLogSink>());
    }
}

} // namespace aqua::gui
