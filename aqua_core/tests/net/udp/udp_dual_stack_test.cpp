// 数据面逐族（双 socket）测试。证明四件事：
//   1. 两族可在**同一端口**共存（这正是 v6_only=true 换来的性质）；
//   2. 两族客户端各自端到端建立 association —— ACK 必须从"收到包的那条 socket"
//      回去，否则客户端永远收不到 ACK（本用例对这种不对称直接超时）；
//   3. broadcast 按会话 endpoint 的族路由：一份 datagram 同时到达两族客户端，
//      且不会出现"在 AF_INET6 socket 上用 AF_INET 目标发送"（EAFNOSUPPORT）；
//   4. 单族绑定失败可降级（该族不服务）而不是拖垮整个数据面；但"无族可服务"时
//      启动必须失败（绑定成功却收不到是最难诊断的状态）。

#include "aqua/net/address/ip_family.h"
#include "aqua/net/udp/network_frame.h"
#include "aqua/net/udp/udp_client.h"
#include "aqua/net/udp/udp_server.h"
#include "aqua/session/session_manager.h"

#include "io_thread.h"

#include <asio.hpp>
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;
using aqua::net::effective_family;
using aqua::net::IpFamily;
using aqua::net::net_error_name;
using aqua::net::UdpBindPlan;
using aqua::net::UdpBindTarget;
using aqua::net::UdpClient;
using aqua::net::UdpServer;
using aqua::session::SessionManager;
using aqua::test::IoThread;

constexpr std::uint32_t kFramesPerSlot = 4;
constexpr std::uint32_t kFrameBytes = 4;
constexpr std::size_t kPayloadBytes = 16; // 4 帧 × 4 字节（PCM_F32LE 1ch）
// 本测试刻意用与 udp_protocol_test 不同的 SSRC：两者互不干扰，
// 也避免"同一个进程里两条流被当成同一条"的误判。
constexpr std::uint32_t kTestSsrc = 0x0BADF00Du;

bool wait_for(const std::function<bool()>& predicate,
    std::chrono::milliseconds timeout = 2s)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(5ms);
    }
    return predicate();
}

std::vector<std::byte> make_payload(std::uint8_t fill)
{
    return std::vector<std::byte>(kPayloadBytes, static_cast<std::byte>(fill));
}

std::vector<std::byte> make_audio(std::uint16_t seq, const std::vector<std::byte>& payload)
{
    return aqua::net::NetworkFrame::audio(
        seq, static_cast<std::uint32_t>(seq) * 480u, kTestSsrc, payload)
        .encode();
}

std::string error_text(const std::optional<aqua::net::NetError>& error)
{
    return error ? std::string(net_error_name(*error)) : std::string("none");
}

TEST(UdpDualStackTest, BindsBothFamiliesOnOnePortAndRoutesByFamily)
{
    asio::io_context io;
    auto sessions = std::make_shared<SessionManager>();
    const auto v4_id = sessions->create_session();
    const auto v6_id = sessions->create_session();
    ASSERT_TRUE(v4_id.has_value());
    ASSERT_TRUE(v6_id.has_value());

    UdpServer server(io, sessions);
    UdpBindPlan plan;
    plan.v4 = UdpBindTarget { "127.0.0.1", false };
    plan.v6 = UdpBindTarget { "::1", false };
    plan.port = 0; // OS 选一次端口，两族共用
    const auto outcome = server.bind(plan);
    ASSERT_TRUE(outcome.served.v4) << "IPv4 bind failed: " << error_text(outcome.v4_error);
    ASSERT_TRUE(outcome.served.v6) << "IPv6 bind failed: " << error_text(outcome.v6_error
        );
    EXPECT_EQ(server.served_families().to_string(), "v4+v6");
    ASSERT_TRUE(server.start());

    const auto port = server.local_endpoint(IpFamily::V4).port();
    ASSERT_NE(port, 0);
    // 同端口共存 + 各族 socket 的确是它自己的族
    EXPECT_EQ(server.local_endpoint(IpFamily::V6).port(), port);
    EXPECT_EQ(effective_family(server.local_endpoint(IpFamily::V4).address()), IpFamily::V4);
    EXPECT_EQ(effective_family(server.local_endpoint(IpFamily::V6).address()), IpFamily::V6);

    std::atomic<int> v4_frames { 0 };
    std::atomic<int> v6_frames { 0 };
    UdpClient client4(io);
    UdpClient client6(io);
    ASSERT_TRUE(client4.set_remote("127.0.0.1", port));
    ASSERT_TRUE(client6.set_remote("::1", port));
    ASSERT_TRUE(client4.start_receive(kFramesPerSlot * kFrameBytes,
        [&](std::uint64_t, std::span<const std::byte>) { v4_frames.fetch_add(1); }));
    ASSERT_TRUE(client6.start_receive(kFramesPerSlot * kFrameBytes,
        [&](std::uint64_t, std::span<const std::byte>) { v6_frames.fetch_add(1); }));
    client4.start_heartbeat(*v4_id, 20ms);
    client6.start_heartbeat(*v6_id, 20ms);

    IoThread thread(io);
    // 两族各自建立 association：若 ACK 从"另一族"的 socket 发出，这里必然超时
    EXPECT_TRUE(wait_for([&] { return sessions->is_connected(*v4_id); })) << "IPv4 association not established";
    EXPECT_TRUE(wait_for([&] { return sessions->is_connected(*v6_id); })) << "IPv6 association not established";

    // 按族路由：一份 datagram 必须同时到达两族客户端
    const auto audio = std::make_shared<const std::vector<std::byte>>(make_audio(1, make_payload(0xAB)));
    const auto recipients = server.broadcast(audio);
    ASSERT_TRUE(recipients.has_value()) << "broadcast threw (multi-family send is likely EAFNOSUPPORT)";
    EXPECT_EQ(*recipients, 2u);
    EXPECT_TRUE(wait_for([&] { return v4_frames.load() > 0 && v6_frames.load() > 0; }));

    client4.stop();
    client6.stop();
    server.stop();
}

TEST(UdpDualStackTest, OptionalFamilyBindFailureDegradesToSingleFamily)
{
    asio::io_context io;
    auto sessions = std::make_shared<SessionManager>();

    UdpServer server(io, sessions);
    UdpBindPlan plan;
    plan.v4 = UdpBindTarget { "127.0.0.1", false };
    // 文档前缀地址，本机不可能持有 —— 绑定必然失败，用于验证降级路径。
    plan.v6 = UdpBindTarget { "2001:db8::1", false };
    plan.port = 0;
    const auto outcome = server.bind(plan);

    EXPECT_TRUE(outcome.served.v4);
    EXPECT_FALSE(outcome.served.v6);
    EXPECT_TRUE(outcome.v6_error.has_value());
    EXPECT_EQ(server.served_families().to_string(), "v4");
    // 一族可用即可启动（INV-5：默认宽松 = 降级 + 告警，不拖垮整体）
    EXPECT_TRUE(server.start());
    EXPECT_EQ(server.local_endpoint(IpFamily::V6).port(), 0u);
    server.stop();
}

TEST(UdpDualStackTest, StartRejectsWhenNoFamilyIsBound)
{
    asio::io_context io;
    auto sessions = std::make_shared<SessionManager>();
    UdpServer server(io, sessions);
    // 未绑定任何族：不得"启动成功但收不到" —— 必须显式失败。
    EXPECT_FALSE(server.start().has_value());
    EXPECT_TRUE(server.served_families().empty());

    UdpBindPlan none; // 两族都关闭
    const auto outcome = server.bind(none);
    EXPECT_TRUE(outcome.served.empty());
    EXPECT_FALSE(outcome.v4_error.has_value());
    EXPECT_FALSE(outcome.v6_error.has_value());
    EXPECT_FALSE(server.start().has_value());
}

TEST(UdpDualStackTest, SingleFamilyAdapterRejectsV4MappedAddress)
{
    asio::io_context io;
    auto sessions = std::make_shared<SessionManager>();
    UdpServer server(io, sessions);
    // v4-mapped 字面量没有自洽的族归宿：归一化后是 v4，但文本形态会让
    // UdpTransport 开出 AF_INET6 socket —— 单族适配器必须拒绝，而不是错误分槽。
    const auto bound = server.bind("::ffff:127.0.0.1", 0);
    EXPECT_FALSE(bound.has_value());
    EXPECT_TRUE(server.served_families().empty());
}

} // namespace
