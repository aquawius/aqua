// diag_block 单元测试：RateCounter 的 T/D/R 语义，以及 Block 的链式 k=v 拼接。
//
// 设计要点（见 aqua/diagnostics/diag_block.h）：
//   - RateCounter::fmt(total) 返回 "T/D/R"：T=累计, D=距上次增量（只增不减，
//     计数器回退时记 0 避免负速率）, R=每秒速率（用真实 steady_clock 间隔计算）。
//   - Block 把若干 field/rate 拼成块的**内部**内容（不含 Diagnostics 加的外层大括号）。

#include "aqua/diagnostics/diag_block.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>

namespace {

using aqua::diagnostics::Block;
using aqua::diagnostics::RateCounter;

TEST(DiagBlockRateCounterTest, FirstSampleShowsTotalOnly)
{
    RateCounter rc;
    // 首拍尚未建立基线：delta=0, rate=0.0。
    EXPECT_EQ(rc.fmt(10), "10/0/0.0");
}

TEST(DiagBlockRateCounterTest, DeltaIsExactAcrossSamples)
{
    RateCounter rc;
    EXPECT_EQ(rc.fmt(100), "100/0/0.0");
    // 紧接上一拍调用：elapsed 不足以产生有意义的速率（≈0），但 delta 精确。
    const std::string second = rc.fmt(150);
    // T=150, D=50（150-100），R 为浮点（结构 "150/50/<rate>"）。
    EXPECT_EQ(second.substr(0, 7), "150/50/");
    EXPECT_NE(second.find('.'), std::string::npos);
}

TEST(DiagBlockRateCounterTest, CounterResetClampsDeltaToZero)
{
    RateCounter rc;
    rc.fmt(1000);
    rc.fmt(1200);
    // 计数器回退（1200 -> 500）：delta 必须为 0，不得出现负速率。
    const std::string reset = rc.fmt(500);
    EXPECT_EQ(reset.substr(0, 6), "500/0/");
}

TEST(DiagBlockBlockTest, EmptyBlockRendersEmpty)
{
    Block b;
    EXPECT_EQ(b.str(), "");
}

TEST(DiagBlockBlockTest, FieldsChainInOrder)
{
    Block b;
    b.field("cap", true)
        .field("n", std::uint64_t { 7 })
        .field("i", std::int64_t { -3 })
        .field("u32", std::uint32_t { 9 })
        .field("u16", std::uint16_t { 3 })
        .field("i32", 4)
        .field("flt", 1.5, 1)
        .field("name", std::string_view("ok"));
    EXPECT_EQ(b.str(), "cap=true n=7 i=-3 u32=9 u16=3 i32=4 flt=1.5 name=ok");
}

TEST(DiagBlockBlockTest, BoolRendersTrueFalse)
{
    Block b;
    b.field("a", true).field("b", false);
    EXPECT_EQ(b.str(), "a=true b=false");
}

TEST(DiagBlockBlockTest, DoubleUsesPrecision)
{
    Block b;
    b.field("x", 3.14159, 2);
    EXPECT_EQ(b.str(), "x=3.14");
}

TEST(DiagBlockBlockTest, RateFieldEmbedsRateCounter)
{
    Block b;
    RateCounter rc;
    rc.fmt(5);
    // 上一拍 total=5，本拍 total=12 -> delta=7。
    b.rate("ev", rc, 12);
    // 上一拍 total=5，本拍 total=12 -> delta=7；rate 取决于两次调用间真实间隔，
    // 不假定定值，只校验 "T/D/" 前缀（T=12, D=7）。
    EXPECT_EQ(b.str().substr(0, 8), "ev=12/7/");
}

} // namespace
