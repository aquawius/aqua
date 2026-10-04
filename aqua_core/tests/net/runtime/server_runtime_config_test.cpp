#include "aqua/runtime/client_runtime.h"
#include "aqua/runtime/server_runtime.h"

#include <asio.hpp>
#include <gtest/gtest.h>

#include <cstdint>
#include <string>

namespace {

// 一份"能走到数据面临界区之前"的合法配置：所有 start() 前的校验都通过，因此每个
// 用例可以精确地把失败归因到它自己改坏的那一项（而不是被别的缺省值提前拦下）。
aqua::runtime::ServerRuntimeConfig make_valid_config()
{
    aqua::runtime::ServerRuntimeConfig cfg;
    cfg.format = aqua::audio::AudioFormat { aqua::audio::AudioEncoding::PCM_F32LE, 2, 48000 };
    cfg.frame_count = 144;
    cfg.rpc_ip = "127.0.0.1";
    cfg.udp_port = 50052;
    cfg.rpc_port = 50052;
    cfg.advertised_udp_ipv4 = "127.0.0.1";
    cfg.advertised_udp_port = 50052;
    return cfg;
}

// 所有配置期拒绝都必须发生在**碰后端之前**：不通告、不绑端口、不启采集。
void expect_start_rejected(const aqua::runtime::ServerRuntimeConfig& cfg)
{
    asio::io_context io;
    auto runtime = std::make_shared<aqua::runtime::ServerRuntime>(io, cfg);
    EXPECT_FALSE(runtime->start());
    EXPECT_EQ(runtime->state(), aqua::runtime::RuntimeState::Stopped);
}

TEST(ServerRuntimeConfigTest, DefaultConfigurationMatchesZeroArgumentServerBaseline)
{
    const aqua::runtime::ServerRuntimeConfig cfg;

    EXPECT_EQ(cfg.rpc_ip, aqua::config::DEFAULT_BIND_IP);
    EXPECT_EQ(cfg.rpc_port, aqua::config::DEFAULT_RPC_PORT);
    // 数据面默认两族都开、共用同一端口 ⇒ "无参数启动即两族都能用"。
    EXPECT_EQ(cfg.udp_ipv4, aqua::config::DEFAULT_UDP_IPV4);
    EXPECT_EQ(cfg.udp_ipv6, aqua::config::DEFAULT_UDP_IPV6);
    EXPECT_EQ(cfg.udp_port, aqua::config::DEFAULT_UDP_PORT);
    EXPECT_TRUE(cfg.advertised_udp_ipv4.empty());
    EXPECT_TRUE(cfg.advertised_udp_ipv6.empty());
    EXPECT_FALSE(cfg.advertised_udp_port.has_value());
    EXPECT_EQ(cfg.capture.source, aqua::audio::AudioCaptureSource::OUTPUT_LOOPBACK);
    EXPECT_FALSE(cfg.capture.route.endpoint_request().has_value());
    EXPECT_FALSE(cfg.format.has_value());
    EXPECT_EQ(cfg.frame_count, 0u);
    EXPECT_EQ(cfg.audio_queue_capacity_slots, aqua::config::DEFAULT_AUDIO_QUEUE_CAPACITY_SLOTS);
}

TEST(ServerRuntimeConfigTest, RejectsInvalidRpcIp)
{
    auto cfg = make_valid_config();
    cfg.rpc_ip = "not-an-ip";
    expect_start_rejected(cfg);
}

TEST(ServerRuntimeConfigTest, RejectsV4MappedRpcIp)
{
    // 控制面绑定同样要求"该族的原生形态"：v4-mapped 字面量会被 gRPC 当成 IPv6 监听
    // 地址，与"这是一个 IPv4 绑定"的意图不符（与数据面同一个坑，只是发生在控制面）。
    auto cfg = make_valid_config();
    cfg.rpc_ip = "::ffff:127.0.0.1";
    expect_start_rejected(cfg);
}

TEST(ServerRuntimeConfigTest, RejectsZeroUdpPort)
{
    // 端口 0 = OS 分配临时端口，被通告给客户端必然是错的（Core 自己守住这条，
    // 不依赖 CLI 是否拦过）。
    auto cfg = make_valid_config();
    cfg.udp_port = 0;
    expect_start_rejected(cfg);
}

TEST(ServerRuntimeConfigTest, RejectsInvalidAdvertisedUdpIpv4)
{
    auto cfg = make_valid_config();
    cfg.advertised_udp_ipv4 = "not-an-ip";
    expect_start_rejected(cfg);
}

// R3：槽内地址族必须与槽族一致 —— 跨族通告在配置期即失败，而不是变成运行期风险。
TEST(ServerRuntimeConfigTest, RejectsCrossFamilyAdvertisedAddress)
{
    auto cfg = make_valid_config();
    cfg.advertised_udp_ipv6 = "192.168.1.5"; // v6 槽里塞 v4 字面量
    expect_start_rejected(cfg);
}

TEST(ServerRuntimeConfigTest, RejectsV4MappedLiteralInIpv4Slot)
{
    // ::ffff:a.b.c.d 归一化后是 V4，但文本形态是 v6 —— 原样交给套接字层会开出
    // AF_INET6 socket，正是"::ffff: 不进入系统"这条不变量禁止的事。
    auto cfg = make_valid_config();
    cfg.udp_ipv4 = "::ffff:127.0.0.1";
    expect_start_rejected(cfg);
}

TEST(ServerRuntimeConfigTest, RejectsV6LiteralAsIpv4BindAddress)
{
    auto cfg = make_valid_config();
    cfg.udp_ipv4 = "::1";
    expect_start_rejected(cfg);
}

TEST(ServerRuntimeConfigTest, RejectsWildcardAdvertisedAddress)
{
    // 通配 sentinel 是**下发**形态而不是配置值：要让客户端用自己的地址，留空即可。
    auto cfg = make_valid_config();
    cfg.advertised_udp_ipv4 = "0.0.0.0";
    expect_start_rejected(cfg);
}

TEST(ServerRuntimeConfigTest, RejectsAdvertiseWithZoneId)
{
    // LL-1：zone 是逐主机本地的接口索引，跨主机传递没有意义。
    auto cfg = make_valid_config();
    cfg.advertised_udp_ipv6 = "fe80::1%24";
    expect_start_rejected(cfg);
}

TEST(ServerRuntimeConfigTest, RejectsLinkLocalIpv6BindWithoutZone)
{
    auto cfg = make_valid_config();
    cfg.udp_ipv6 = "fe80::1";
    expect_start_rejected(cfg);
}

TEST(ServerRuntimeConfigTest, RejectsAdvertiseForDisabledFamily)
{
    auto cfg = make_valid_config();
    cfg.udp_ipv4.clear(); // = --udp-ipv4 none
    cfg.advertised_udp_ipv4 = "192.168.1.5";
    expect_start_rejected(cfg);
}

TEST(ServerRuntimeConfigTest, RejectsBothFamiliesDisabled)
{
    auto cfg = make_valid_config();
    cfg.udp_ipv4.clear();
    cfg.udp_ipv6.clear();
    cfg.advertised_udp_ipv4.clear();
    expect_start_rejected(cfg);
}

TEST(ServerRuntimeConfigTest, RejectsAudioPayloadAboveUdpBudgetBeforeBackendSetup)
{
    auto cfg = make_valid_config();
    cfg.frame_count = static_cast<std::uint32_t>(
        aqua::config::UDP_AUDIO_PAYLOAD_BYTES / cfg.format->frame_bytes() + 1);
    expect_start_rejected(cfg);
}

TEST(ClientRuntimeConfigTest, DefaultConfigurationKeepsOptionalClientSettings)
{
    const aqua::runtime::ClientRuntimeConfig cfg;

    EXPECT_EQ(cfg.jb_capacity_slots, aqua::config::JB_DEFAULT_CAPACITY_SLOTS);
    EXPECT_EQ(cfg.rpc_port, aqua::config::DEFAULT_RPC_PORT);
    EXPECT_EQ(cfg.rpc_ip, "127.0.0.1");
    EXPECT_EQ(cfg.client_name, "aqua-client");
    EXPECT_FALSE(cfg.playback.route.endpoint_request().has_value());
}

} // namespace
