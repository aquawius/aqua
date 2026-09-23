#ifndef AQUA_DIAGNOSTICS_SNAPSHOT_LINE_H
#define AQUA_DIAGNOSTICS_SNAPSHOT_LINE_H

// 诊断行组装器：把若干个"模块块"拼成一行 Debug 日志。
//
// 职责边界（**不拥有任何运行态**）：
//   - 只管"怎么打印"：注册 name -> 字符串的 source，拼成
//     `Client diag: name{...} name{...} ...` 一行；
//   - 块的**内部内容**由 SnapshotView（snapshot_view.h）渲染，
//     其中的 `T/D/R` 由 field_block.h 的 RateCounter 产出；
//   - Debug 未启用时整行跳过，连 source 都不求值——诊断 getter
//     跨多个 atomic 读取，不看 debug 日志就不该付这份成本。
//
// **CLI-only**：只有 aqua_client_cli / aqua_server_cli 走这条路径。
// C API / JNI / Android 只消费 ClientDiagnosticsSnapshot /
// ServerDiagnosticsSnapshot 这两份快照契约，**不经过本类**：快照是
// 无时间状态的 POD，速率在 App 侧由 `AquaRates.kt` 自己差分。
//
// 读法见 aqua_core/doc/diagnostics.md。

#include <functional>
#include <string>
#include <vector>

namespace aqua::diagnostics {

class SnapshotLine {
public:
    using SourceFn = std::function<std::string()>;

    explicit SnapshotLine(std::string component_name);

    SnapshotLine(const SnapshotLine&) = delete;
    SnapshotLine& operator=(const SnapshotLine&) = delete;

    // 注册一个模块块。name 是块名（外层大括号的名字），
    // fn 产出块的**内部**内容（不含大括号）。
    void add_source(std::string name, SourceFn fn);

    // 组装并输出一行，例如：
    //   Client diag: state{running} net{...} jb{...} playback{...}
    // Debug 关闭时立即返回，不调用任何 source。
    void log_debug() const;

private:
    struct Source {
        std::string name;
        SourceFn fn;
    };

    std::string component_name_;
    std::vector<Source> sources_;
};

} // namespace aqua::diagnostics

#endif // AQUA_DIAGNOSTICS_SNAPSHOT_LINE_H
