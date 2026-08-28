#include "aqua/runtime/server_runtime.h"

#include <asio.hpp>
#include <gtest/gtest.h>

#include <cstdint>

namespace {

aqua::runtime::ServerRuntimeConfig make_valid_config()
{
    aqua::runtime::ServerRuntimeConfig cfg;
    cfg.format = aqua::audio::AudioFormat { aqua::audio::AudioEncoding::PCM_F32LE, 2, 48000 };
    cfg.frame_count = 144;
    cfg.udp_bind_ip = "127.0.0.1";
    cfg.udp_port = 0;
    cfg.rpc_bind_ip = "127.0.0.1";
    cfg.rpc_port = 50052;
    cfg.advertised_udp_address = "127.0.0.1";
    return cfg;
}

TEST(ServerRuntimeConfigTest, RejectsInvalidAdvertisedUdpAddressBeforeBackendSetup)
{
    asio::io_context io;
    auto cfg = make_valid_config();
    cfg.advertised_udp_address = "not-an-ip";

    auto runtime = std::make_shared<aqua::runtime::ServerRuntime>(io, cfg);
    EXPECT_FALSE(runtime->start());
    EXPECT_EQ(runtime->state(), aqua::runtime::RuntimeState::Stopped);
}

TEST(ServerRuntimeConfigTest, RejectsAudioPayloadAboveUdpBudgetBeforeBackendSetup)
{
    asio::io_context io;
    auto cfg = make_valid_config();
    cfg.frame_count = static_cast<std::uint32_t>(
        aqua::config::UDP_AUDIO_PAYLOAD_BYTES / cfg.format.frame_bytes() + 1);

    auto runtime = std::make_shared<aqua::runtime::ServerRuntime>(io, cfg);
    EXPECT_FALSE(runtime->start());
    EXPECT_EQ(runtime->state(), aqua::runtime::RuntimeState::Stopped);
}

} // namespace
