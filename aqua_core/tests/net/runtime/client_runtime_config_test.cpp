#include "aqua/runtime/client_runtime.h"

#include <asio.hpp>
#include <gtest/gtest.h>

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
