//
// Created by aquawius on 25-1-22.
//

#ifndef FORMATTER_H
#define FORMATTER_H

#include <spdlog/fmt/fmt.h>
#include <sstream>
#include <thread>

// 自定义 formatter
template <>
struct fmt::formatter<std::thread::id> {
    // parse 方法可按需实现；此处简单返回 ctx.begin()，表示不进行额外的解析
    template <typename ParseContext>
    constexpr auto parse(ParseContext& ctx)
    {
        return ctx.begin();
    }

    template <typename FormatContext>
    auto format(const std::thread::id& id, FormatContext& ctx) const
    {
        std::ostringstream oss;
        oss << id; // 将 thread::id 转成字符串
        // 使用 fmt::format_to 输出到 ctx.out()
        return fmt::format_to(ctx.out(), "{}", oss.str());
    }
};

#endif // FORMATTER_H
