#ifndef AQUA_DIAGNOSTICS_DIAGNOSTICS_H
#define AQUA_DIAGNOSTICS_DIAGNOSTICS_H

// 诊断报告器：注册若干命名指标源，print() 逐源打印一行日志。
// 由外部定时器（如 1s steady_timer）周期调用；不在实时路径，源函数可自由做
// 格式化 / 字符串构造（std::format、读 stats 快照等）。

#include <functional>
#include <string>
#include <vector>

namespace aqua::diagnostics {

class Diagnostics {
public:
    // 指标源：返回该模块的一行统计文本（不含模块名）。
    using SourceFn = std::function<std::string()>;

    Diagnostics() = default;

    // 注册一个命名指标源。
    void add_source(std::string name, SourceFn fn);

    // 打印所有来源（每源一行）。源函数抛异常会被吞掉，不影响主流程。
    void print() const;

private:
    struct Source {
        std::string name;
        SourceFn fn;
    };
    std::vector<Source> sources_;
};

} // namespace aqua::diagnostics

#endif // AQUA_DIAGNOSTICS_DIAGNOSTICS_H
