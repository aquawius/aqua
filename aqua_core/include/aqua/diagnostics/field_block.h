#ifndef AQUA_DIAGNOSTICS_FIELD_BLOCK_H
#define AQUA_DIAGNOSTICS_FIELD_BLOCK_H

// 诊断紧凑渲染助手。
//
// 设计动机：诊断行一度因为"每个计数器都展开成 total/delta/rate 三个长字段"
// 而膨胀到 2.5KB、不可读。本项目把所有诊断量按**模块**聚合成一段段
// `module{ k=v k=v ... }` 块（server 侧 audio/capture/pktz/queue/dsp/net/sess，
// client 侧 net/jb/jc/pb/stream），每段块由对应模块的 `SnapshotView` 渲染。
//
// RateCounter 持有跨快照的持久状态，负责把"累计值"换算成人类关心的
// "这一秒涨了多少 / 现在每秒多少"，渲染为紧凑的 `T/D/R`
// （T=total 累计, D=距上次快照增量, R=每秒速率）。Block 则负责把若干
// `k=v` 拼成一个块的内部内容（外层大括号由 SnapshotLine 加）。
//
// field() 用模板覆盖全部整型 / 枚举 / 浮点，避免为每种宽度补重载：
//   - 整型与 enum class 走 `field(key, T)`，枚举按底层整型打印；
//   - 浮点走 `field(key, T, precision=2)`，默认 2 位小数、可显式指定；
//   - bool 打印 true/false，std::string_view 原样输出（std::string 可隐式转换）。
// 读法见 aqua_core/doc/diagnostics.md。

#include <chrono>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <type_traits>

namespace aqua::diagnostics {

// 紧凑速率计数器：跨拍 delta/rate 的单一真相源。
// 首拍只显示 `T/0/0`（尚未建立基线）；之后 delta 只增不减（计数器回退时记 0，
// 避免负速率），rate 用真实 steady_clock elapsed 计算，不假定 timer 精确。
class RateCounter {
public:
    // 喂入新 total，返回 "T/D/R"。mutable：被 FieldBlock::rate 在 const 渲染方法内调用。
    std::string fmt(std::uint64_t total) const;

private:
    mutable bool initialized_ = false;
    mutable std::uint64_t last_ = 0;
    mutable std::chrono::steady_clock::time_point last_sample_;
};

// 诊断块构建器：链式追加字段，最终产出 "k=v k=v ..." 字符串。
// 用法：
//   FieldBlock b;
//   b.field("cap", true).field("F", 175).field("ratio", 0.42, 3).rate("ev", rc, events);
//   return b.str();   // -> "cap=true F=175 ratio=0.420 ev=77/39/31.3"
//
// 全部方法 header-only 内联：模板 field 需定义在头文件，其余方法与之同放以避免
// 头文件/源文件重复定义。
class FieldBlock {
public:
    // 整数 / 枚举：单个模板覆盖所有整型宽度与 enum class，
    // 不再为 uint16/uint32/int64/... 逐个补重载。
    template <typename T, std::enable_if_t<(std::is_integral_v<T> && !std::is_same_v<T, bool>) || std::is_enum_v<T>, int> = 0>
    FieldBlock& field(std::string_view key, T v)
    {
        append_key(key);
        if constexpr (std::is_enum_v<T>) {
            out_ += std::to_string(static_cast<std::underlying_type_t<T>>(v));
        } else {
            out_ += std::to_string(v);
        }
        return *this;
    }

    FieldBlock& field(std::string_view key, bool v)
    {
        append_key(key);
        out_ += v ? "true" : "false";
        return *this;
    }

    // C 字符串：必须有这个精确匹配重载，否则 const char* 会因上面的模板约束
    // （仅整型/枚举）掉进 bool 重载，把名字打成 true（实测 src=/path=/err=
    // 长期全是 true）。string_view 形参版救不了它（用户定义转换输给 bool
    // 的标准转换）。调用点仍建议显式包 string_view，双保险。
    FieldBlock& field(std::string_view key, const char* v)
    {
        return field(key, std::string_view(v != nullptr ? v : ""));
    }

    // 浮点：默认 2 位小数，可显式指定精度。
    template <typename T, std::enable_if_t<std::is_floating_point_v<T>, int> = 0>
    FieldBlock& field(std::string_view key, T v, int precision = 2)
    {
        append_key(key);
        out_ += std::format("{:.{}f}", static_cast<double>(v), precision);
        return *this;
    }

    FieldBlock& field(std::string_view key, std::string_view v)
    {
        append_key(key);
        out_ += v;
        return *this;
    }

    // 速率字段：k=T/D/R（依赖 RateCounter 的跨拍状态，rc 为 const 引用）。
    FieldBlock& rate(std::string_view key, const RateCounter& rc, std::uint64_t total)
    {
        append_key(key);
        out_ += rc.fmt(total);
        return *this;
    }

    std::string str() const { return out_; }

private:
    void append_key(std::string_view key)
    {
        if (!out_.empty())
            out_ += ' ';
        out_ += key;
        out_ += '=';
    }

    std::string out_;
};

} // namespace aqua::diagnostics

#endif // AQUA_DIAGNOSTICS_FIELD_BLOCK_H
