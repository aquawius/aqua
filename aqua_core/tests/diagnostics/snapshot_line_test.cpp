#include "aqua/diagnostics/snapshot_line.h"

#include <gtest/gtest.h>

#include "aqua/logger/logger.h"

#include <cstdint>
#include <stdexcept>
#include <string>

namespace {

using aqua::diagnostics::SnapshotLine;

class SnapshotLineTest : public ::testing::Test {
protected:
    void TearDown() override
    {
        aqua::set_log_level(aqua::LogLevel::Info);
    }
};

TEST_F(SnapshotLineTest, DebugDisabledDoesNotEvaluateSources)
{
    SnapshotLine line("Client");
    int evaluations = 0;
    line.add_source("state", [&]() {
        ++evaluations;
        return std::string("state=running");
    });

    aqua::set_log_level(aqua::LogLevel::Info);
    line.log_debug();

    EXPECT_EQ(evaluations, 0);
}

TEST_F(SnapshotLineTest, SourcesAreEvaluatedAtDebugLevel)
{
    SnapshotLine line("Client");
    int evaluations = 0;
    line.add_source("state", [&]() {
        ++evaluations;
        return std::string("state=running");
    });

    aqua::set_log_level(aqua::LogLevel::Debug);
    line.log_debug();

    EXPECT_EQ(evaluations, 1);
}

TEST_F(SnapshotLineTest, TraceLevelAlsoEnablesDebugDiagnostics)
{
    SnapshotLine line("Server");
    int evaluations = 0;
    line.add_source("state", [&]() {
        ++evaluations;
        return std::string("state=running");
    });

    aqua::set_log_level(aqua::LogLevel::Trace);
    line.log_debug();

    // Trace is the most verbose threshold, so Debug diagnostics are also enabled.
    EXPECT_EQ(evaluations, 1);
}

TEST_F(SnapshotLineTest, ThrowingSourceDoesNotAbortSnapshot)
{
    SnapshotLine line("Server");
    int good_evaluations = 0;
    line.add_source("bad", []() -> std::string { throw std::runtime_error("boom"); });
    line.add_source("good", [&]() {
        ++good_evaluations;
        return std::string("k=1");
    });

    aqua::set_log_level(aqua::LogLevel::Debug);
    line.log_debug();

    EXPECT_EQ(good_evaluations, 1);
}

} // namespace
