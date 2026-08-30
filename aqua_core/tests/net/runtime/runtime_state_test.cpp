#include "aqua/runtime/runtime_state.h"

#include <gtest/gtest.h>

namespace {

TEST(RuntimeStateTest, NamesAreStable)
{
    EXPECT_STREQ(aqua::runtime::runtime_state_name(aqua::runtime::RuntimeState::Created), "created");
    EXPECT_STREQ(aqua::runtime::runtime_state_name(aqua::runtime::RuntimeState::Starting), "starting");
    EXPECT_STREQ(aqua::runtime::runtime_state_name(aqua::runtime::RuntimeState::Running), "running");
    EXPECT_STREQ(aqua::runtime::runtime_state_name(aqua::runtime::RuntimeState::Degraded), "degraded");
    EXPECT_STREQ(aqua::runtime::runtime_state_name(aqua::runtime::RuntimeState::Stopping), "stopping");
    EXPECT_STREQ(aqua::runtime::runtime_state_name(aqua::runtime::RuntimeState::Stopped), "stopped");
}

TEST(RuntimeStateTest, StateNamesAreNonempty)
{
    for (const auto state : {
             aqua::runtime::RuntimeState::Created,
             aqua::runtime::RuntimeState::Starting,
             aqua::runtime::RuntimeState::Running,
             aqua::runtime::RuntimeState::Degraded,
             aqua::runtime::RuntimeState::Stopping,
             aqua::runtime::RuntimeState::Stopped }) {
        EXPECT_NE(*aqua::runtime::runtime_state_name(state), '\0');
    }
}

} // namespace
