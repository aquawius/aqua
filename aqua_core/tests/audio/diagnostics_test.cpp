#include "aqua/diagnostics/diagnostics.h"

#include <gtest/gtest.h>

#include <stdexcept>
#include <string>

namespace {

using aqua::diagnostics::Diagnostics;

TEST(DiagnosticsTest, EmptySourcesPrintIsNoop)
{
    Diagnostics diag;
    diag.print(); // 无来源，不崩溃
    SUCCEED();
}

TEST(DiagnosticsTest, ThrowingSourceIsCaught)
{
    Diagnostics diag;
    diag.add_source("boom", []() -> std::string { throw std::runtime_error("boom"); });
    diag.print(); // 异常被吞，不崩溃
    SUCCEED();
}

TEST(DiagnosticsTest, MultipleSourcesPrint)
{
    Diagnostics diag;
    diag.add_source("a", []() { return std::string("k=1"); });
    diag.add_source("b", []() { return std::string("v=2"); });
    diag.print();
    SUCCEED();
}

} // namespace
