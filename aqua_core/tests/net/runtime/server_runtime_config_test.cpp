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

// 取一个当前空闲的 UDP 端口（绑 0 → 读实际端口 → 立刻释放），避免固定端口撞车。
std::uint16_t find_free_udp_port()
{
    asio::io_context io;
    asio::ip::udp::socket probe(io,
        asio::ip::udp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    return probe.local_endpoint().port();
}

// INV-5 的**严格**半边：pin 了具体地址的族绑定失败 ⇒ 启动必须失败（不静默换地址）。
// 每个用例都断言"另一族**确实绑上了**"——这是"真的走到了 bind 阶段"的证据；否则本用例
// 会因为测试环境缺采集后端而在更早的地方返回 false，变成一条永远绿但毫无意义的测试。
TEST(ServerRuntimeConfigTest, RejectsStartWhenPinnedIpv4BindFails)
{
    auto cfg = make_valid_config();
    cfg.udp_port = find_free_udp_port();
    cfg.udp_ipv4 = "192.0.2.1"; // TEST-NET-1：本机不可能持有 ⇒ 绑定必然失败
    cfg.advertised_udp_ipv4.clear(); // 让通告跟随绑定地址，避免干扰校验
    // udp_ipv6 保持默认通配 ::（能绑上），用它的成功证明流程走到了 bind。

    asio::io_context io;
    auto runtime = std::make_shared<aqua::runtime::ServerRuntime>(io, cfg);
    EXPECT_FALSE(runtime->start());
    EXPECT_TRUE(runtime->udp_served_families().v6) << "start() 未走到 bind 阶段，本用例失去意义";
    EXPECT_FALSE(runtime->udp_served_families().v4);
}

TEST(ServerRuntimeConfigTest, RejectsStartWhenPinnedIpv6BindFails)
{
    auto cfg = make_valid_config();
    cfg.udp_port = find_free_udp_port();
    cfg.udp_ipv4 = "127.0.0.1"; // 具体地址（pin）、能绑上
    cfg.udp_ipv6 = "2001:db8::1"; // 文档前缀地址 ⇒ 绑定必然失败；pin ⇒ 致命
    cfg.advertised_udp_ipv4.clear();
    cfg.advertised_udp_ipv6.clear();

    asio::io_context io;
    auto runtime = std::make_shared<aqua::runtime::ServerRuntime>(io, cfg);
    EXPECT_FALSE(runtime->start());
    EXPECT_TRUE(runtime->udp_served_families().v4) << "start() 未走到 bind 阶段，本用例失去意义";
    EXPECT_FALSE(runtime->udp_served_families().v6);
}

TEST(ServerRuntimeConfigTest, RejectsWildcardAdvertisedIpv6)
{
    // v6 槽的通配 sentinel 同理：要走"让客户端用自己地址"就留空，不要把 :: 写进配置。
    auto cfg = make_valid_config();
    cfg.advertised_udp_ipv6 = "::";
    expect_start_rejected(cfg);
}

TEST(ServerRuntimeConfigTest, RejectsZeroAdvertisedUdpPort)
{
    auto cfg = make_valid_config();
    cfg.advertised_udp_port = 0;
    expect_start_rejected(cfg);
}

TEST(ServerRuntimeConfigTest, RejectsUdpIpv6BindWithV4MappedLiteral)
{
    auto cfg = make_valid_config();
    cfg.udp_ipv6 = "::ffff:127.0.0.1";
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

// 族一致性：槽内地址族必须与槽族一致 —— 跨族通告在配置期即失败，而不是变成运行期风险。
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
    // INV-10：zone 是逐主机本地的接口索引，跨主机传递没有意义。
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
