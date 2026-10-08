#include "aqua/runtime/client_runtime.h"

#include "aqua/net/grpc/grpc_config.h"

#include <asio.hpp>
#include <gtest/gtest.h>

#include <string>

namespace {

aqua::runtime::ClientRuntimeConfig make_valid_config()
{
    aqua::runtime::ClientRuntimeConfig cfg;
    cfg.rpc_ip = "127.0.0.1";
    cfg.rpc_port = 50052;
    cfg.client_name = "test-client";
    return cfg;
}

TEST(ClientRuntimeConfigTest, RejectsOversizedJitterBuffer)
{
    asio::io_context io;
    auto cfg = make_valid_config();
    cfg.jb_capacity_slots = aqua::config::JB_MAX_CAPACITY_SLOTS + 1;

    aqua::runtime::ClientRuntime runtime(io, cfg);
    EXPECT_FALSE(runtime.start());
    EXPECT_EQ(runtime.state(), aqua::runtime::RuntimeState::Stopped);
}

TEST(ClientRuntimeConfigTest, RejectsInvalidServerAddress)
{
    asio::io_context io;
    auto cfg = make_valid_config();
    cfg.rpc_ip = "not-an-ip";

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

// client_name 是协议约束（服务端 Connect 硬拒 >GRPC_MAX_CLIENT_NAME_BYTES），
// 不变量必须在 Core ——C API / JNI 都不经过 CLI parser。直接测校验函数，
// 否则用例会假绿：没有 server 时 start() 本来就会因连不上而失败。
TEST(ClientRuntimeConfigTest, IdentityValidatorRejectsEmptyAndOversizedClientName)
{
    auto empty = make_valid_config();
    empty.client_name.clear();
    const auto empty_error = aqua::runtime::validate_client_identity(empty);
    ASSERT_TRUE(empty_error.has_value());
    EXPECT_NE(empty_error->find("empty"), std::string::npos) << *empty_error;

    auto oversized = make_valid_config();
    oversized.client_name = std::string(aqua::config::GRPC_MAX_CLIENT_NAME_BYTES + 1, 'x');
    const auto oversized_error = aqua::runtime::validate_client_identity(oversized);
    ASSERT_TRUE(oversized_error.has_value());
    EXPECT_NE(oversized_error->find("at most"), std::string::npos) << *oversized_error;
}

TEST(ClientRuntimeConfigTest, IdentityValidatorAcceptsBoundaryLengths)
{
    // 恰好等于上限合法（服务端是 > 才拒）。
    auto at_limit = make_valid_config();
    at_limit.client_name = std::string(aqua::config::GRPC_MAX_CLIENT_NAME_BYTES, 'x');
    EXPECT_FALSE(aqua::runtime::validate_client_identity(at_limit).has_value());

    auto one_byte_over = make_valid_config();
    one_byte_over.client_name = std::string(aqua::config::GRPC_MAX_CLIENT_NAME_BYTES + 1, 'x');
    EXPECT_TRUE(aqua::runtime::validate_client_identity(one_byte_over).has_value());
}

TEST(ClientRuntimeConfigTest, StartRejectsOversizedClientName)
{
    asio::io_context io;
    auto cfg = make_valid_config();
    cfg.client_name = std::string(aqua::config::GRPC_MAX_CLIENT_NAME_BYTES + 1, 'x');

    aqua::runtime::ClientRuntime runtime(io, cfg);
    EXPECT_FALSE(runtime.start());
    EXPECT_EQ(runtime.state(), aqua::runtime::RuntimeState::Stopped);
}

TEST(ClientRuntimeConfigTest, EndpointValidatorAcceptsConcreteLiterals)
{
    auto v4 = make_valid_config();
    EXPECT_FALSE(aqua::runtime::validate_client_endpoint(v4).has_value());

    auto v6 = make_valid_config();
    v6.rpc_ip = "::1";
    EXPECT_FALSE(aqua::runtime::validate_client_endpoint(v6).has_value());
}

// INV-8 的反向用例。直接测校验函数（而不是只测 start() 返回 false）——否则用例会假绿：
// 没有 server 时 start() 本来就会因为连不上而失败，无法区分"被校验拦下"与"网络失败"。
TEST(ClientRuntimeConfigTest, EndpointValidatorRejectsWildcardRpcIp)
{
    for (const auto* wildcard : { "0.0.0.0", "::" }) {
        auto cfg = make_valid_config();
        cfg.rpc_ip = wildcard;
        const auto error = aqua::runtime::validate_client_endpoint(cfg);
        ASSERT_TRUE(error.has_value()) << wildcard;
        EXPECT_NE(error->find("concrete"), std::string::npos) << *error;
    }
}

TEST(ClientRuntimeConfigTest, EndpointValidatorRejectsV4MappedRpcIp)
{
    auto cfg = make_valid_config();
    cfg.rpc_ip = "::ffff:127.0.0.1";
    const auto error = aqua::runtime::validate_client_endpoint(cfg);
    ASSERT_TRUE(error.has_value());
    EXPECT_NE(error->find("v4-mapped"), std::string::npos) << *error;
}

TEST(ClientRuntimeConfigTest, EndpointValidatorRejectsNonLiteralAndZeroPort)
{
    auto literal = make_valid_config();
    literal.rpc_ip = "not-an-ip";
    EXPECT_TRUE(aqua::runtime::validate_client_endpoint(literal).has_value());

    auto port = make_valid_config();
    port.rpc_port = 0;
    EXPECT_TRUE(aqua::runtime::validate_client_endpoint(port).has_value());
}

TEST(ClientRuntimeConfigTest, EndpointValidatorRequiresZoneForLinkLocalRpcIp)
{
    // link-local 缺 zone 时连控制面都发不出去（内核对无 scope 的 link-local 一律 EINVAL），
    // 更没有 zone 可以借给数据面。这条兑现 modules/address.md 的"两端都缺 zone 就报明确错误"，
    // 且与 ServerRuntime 数据面 bind 侧的校验对称。
    auto missing = make_valid_config();
    missing.rpc_ip = "fe80::1";
    const auto error = aqua::runtime::validate_client_endpoint(missing);
    ASSERT_TRUE(error.has_value());
    EXPECT_NE(error->find("zone"), std::string::npos) << *error;

    // 带上 zone（Windows 上是数字 ifIndex）就合法。
    auto zoned = make_valid_config();
    zoned.rpc_ip = "fe80::1%24";
    EXPECT_FALSE(aqua::runtime::validate_client_endpoint(zoned).has_value());
}

TEST(ClientRuntimeConfigTest, StartRejectsWildcardServerAddress)
{
    // CLI parser 会拦通配 rpc_ip，但 C API / JNI 不经过它 —— 这条守卫必须由 Core 自己提供。
    asio::io_context io;
    auto cfg = make_valid_config();
    cfg.rpc_ip = "0.0.0.0";

    aqua::runtime::ClientRuntime runtime(io, cfg);
    EXPECT_FALSE(runtime.start());
    EXPECT_EQ(runtime.state(), aqua::runtime::RuntimeState::Stopped);
}

} // namespace
