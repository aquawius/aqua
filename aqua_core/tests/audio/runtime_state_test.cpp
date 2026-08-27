#include "aqua/audio/audio_error.h"
#include "aqua/runtime/runtime_state.h"

#include <gtest/gtest.h>

TEST(RuntimeStateTest, NamesAreStableAndComplete)
{
    EXPECT_EQ(aqua::runtime::runtime_state_name(aqua::runtime::RuntimeState::Created), "created");
    EXPECT_EQ(aqua::runtime::runtime_state_name(aqua::runtime::RuntimeState::Starting), "starting");
    EXPECT_EQ(aqua::runtime::runtime_state_name(aqua::runtime::RuntimeState::Running), "running");
    EXPECT_EQ(aqua::runtime::runtime_state_name(aqua::runtime::RuntimeState::Degraded), "degraded");
    EXPECT_EQ(aqua::runtime::runtime_state_name(aqua::runtime::RuntimeState::Stopping), "stopping");
    EXPECT_EQ(aqua::runtime::runtime_state_name(aqua::runtime::RuntimeState::Stopped), "stopped");
}

TEST(RuntimeStateTest, AudioErrorNamesAreStable)
{
    EXPECT_EQ(aqua::audio::audio_error_name(aqua::audio::AudioError::None), "none");
    EXPECT_EQ(aqua::audio::audio_error_name(aqua::audio::AudioError::DeviceDisconnected), "device_disconnected");
    EXPECT_EQ(aqua::audio::audio_error_name(aqua::audio::AudioError::BackendFailed), "backend_failed");
}
