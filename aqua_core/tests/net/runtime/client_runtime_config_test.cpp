#include "aqua/runtime/client_runtime.h"

#include <asio.hpp>
#include <gtest/gtest.h>

namespace {

aqua::runtime::ClientRuntimeConfig make_valid_config()
{
    aqua::runtime::ClientRuntimeConfig cfg;
    cfg.server_ip = "127.0.0.1";
    cfg.rpc_port = 50052;
    cfg.client_name = "test-client";
    return cfg;
}

TEST(ClientRuntimeConfigTest, RejectsOversizedJitterBuffer)
{
    asio::io_context io;
    auto cfg = make_valid_config();
    cfg.jitter_buffer_slots = aqua::runtime::limits::MAX_JITTER_BUFFER_SLOTS + 1;

    aqua::runtime::ClientRuntime runtime(io, cfg);
    EXPECT_FALSE(runtime.start());
    EXPECT_EQ(runtime.state(), aqua::runtime::RuntimeState::Stopped);
}

TEST(ClientRuntimeConfigTest, RejectsInvalidServerAddress)
{
    asio::io_context io;
    auto cfg = make_valid_config();
    cfg.server_ip = "not-an-ip";

    aqua::runtime::ClientRuntime runtime(io, cfg);
    EXPECT_FALSE(runtime.start());
    EXPECT_EQ(runtime.state(), aqua::runtime::RuntimeState::Stopped);
}

TEST(ClientRuntimeConfigTest, RejectsEmptyClientName)
{
    asio::io_context io;
    auto cfg = make_valid_config();
    cfg.client_name.clear();

    aqua::runtime::ClientRuntime runtime(io, cfg);
    EXPECT_FALSE(runtime.start());
    EXPECT_EQ(runtime.state(), aqua::runtime::RuntimeState::Stopped);
}

} // namespace
