#include "aqua/diagnostics/diagnostics.h"

#include <gtest/gtest.h>

#include "aqua/logger/logger.h"

#include <stdexcept>
#include <string>
#include <cstdint>
#include <thread>

namespace {

using aqua::diagnostics::Diagnostics;

class DiagnosticsTest : public ::testing::Test {
protected:
    void TearDown() override
    {
        aqua::set_log_level(aqua::LogLevel::Info);
    }
};

TEST_F(DiagnosticsTest, DebugDisabledDoesNotEvaluateSources)
{
    Diagnostics diag("Client");
    int evaluations = 0;
    diag.add_source("state", [&]() {
        ++evaluations;
        return std::string("state=running");
    });

    aqua::set_log_level(aqua::LogLevel::Info);
    diag.log_debug();

    EXPECT_EQ(evaluations, 0);
}

TEST_F(DiagnosticsTest, SourcesAreEvaluatedAtDebugLevel)
{
    Diagnostics diag("Client");
    int evaluations = 0;
    diag.add_source("state", [&]() {
        ++evaluations;
        return std::string("state=running");
    });

    aqua::set_log_level(aqua::LogLevel::Debug);
    diag.log_debug();

    EXPECT_EQ(evaluations, 1);
}

TEST_F(DiagnosticsTest, TraceLevelAlsoEnablesDebugDiagnostics)
{
    Diagnostics diag("Server");
    int evaluations = 0;
    diag.add_source("state", [&]() {
        ++evaluations;
        return std::string("state=running");
    });

    aqua::set_log_level(aqua::LogLevel::Trace);
    diag.log_debug();

    // Trace is the most verbose threshold, so Debug diagnostics are also enabled.
    EXPECT_EQ(evaluations, 1);
}

TEST_F(DiagnosticsTest, CounterSourceCanBeRegistered)
{
    Diagnostics diag("Test");
    std::uint64_t counter = 0;
    diag.add_counter("events", [&counter]() { return counter; });

    aqua::set_log_level(aqua::LogLevel::Debug);
    diag.log_debug();
    counter = 100;
    diag.log_debug();

    SUCCEED();
}

TEST_F(DiagnosticsTest, ThrowingSourceDoesNotAbortSnapshot)
{
    Diagnostics diag("Server");
    int good_evaluations = 0;
    diag.add_source("bad", []() -> std::string { throw std::runtime_error("boom"); });
    diag.add_source("good", [&]() {
        ++good_evaluations;
        return std::string("k=1");
    });

    aqua::set_log_level(aqua::LogLevel::Debug);
    diag.log_debug();

    EXPECT_EQ(good_evaluations, 1);
}

} // namespace
