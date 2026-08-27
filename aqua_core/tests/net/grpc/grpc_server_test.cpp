#include "aqua/net/grpc/grpc_server.h"
#include "aqua/net/grpc/grpc_client.h"
#include "aqua/net/grpc/grpc_include.h"

#include <gtest/gtest.h>
#include <asio.hpp>

#include <chrono>
#include <cstdint>
#include <thread>

namespace {

TEST(GrpcServerTest, RejectsInvalidBindAddress)
{
    aqua::session::SessionManager sessions;
    aqua::audio::AudioFormat format;
    format.encoding = aqua::audio::AudioEncoding::PCM_F32LE;
    format.channels = 2;
    format.sample_rate = 48000;

    aqua::grpc::GrpcServer server(
        sessions, format, 480, "not-an-ip", 50051, { "127.0.0.1", 9999 });

    EXPECT_FALSE(server.is_running());
}

TEST(GrpcServerTest, ShutdownBeforeRunIsSafe)
{
    aqua::session::SessionManager sessions;
    aqua::audio::AudioFormat format;
    format.encoding = aqua::audio::AudioEncoding::PCM_F32LE;
    format.channels = 2;
    format.sample_rate = 48000;

    // Port 0 lets the OS choose a free gRPC listening port. We only verify startup/shutdown
    // lifecycle here because GrpcServer intentionally does not expose its selected port.
    aqua::grpc::GrpcServer server(
        sessions, format, 480, "127.0.0.1", 0, { "127.0.0.1", 9999 });

    // Shutdown is documented as safe from any thread and is allowed before run().
    server.shutdown();
    EXPECT_FALSE(server.is_running());
}

} // namespace

namespace {

std::uint16_t find_free_tcp_port()
{
    asio::io_context io;
    asio::ip::tcp::acceptor acceptor(io, asio::ip::tcp::endpoint(
        asio::ip::make_address("127.0.0.1"), 0));
    return acceptor.local_endpoint().port();
}

TEST(GrpcServerTest, ConnectAndDisconnectRoundTrip)
{
    aqua::session::SessionManager sessions;
    aqua::audio::AudioFormat format;
    format.encoding = aqua::audio::AudioEncoding::PCM_F32LE;
    format.channels = 2;
    format.sample_rate = 48000;

    const auto port = find_free_tcp_port();
    aqua::grpc::GrpcServer server(
        sessions, format, 480, "127.0.0.1", port, { "127.0.0.1", 50051 });
    EXPECT_FALSE(server.is_running());

    std::thread server_thread([&server] { server.run(); });
    for (int i = 0; i < 100 && !server.is_running(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    if (!server.is_running()) {
        server.shutdown();
        if (server_thread.joinable()) {
            server_thread.join();
        }
        FAIL() << "GrpcServer did not enter running state";
    }

    aqua::grpc::GrpcClient client;
    ASSERT_TRUE(client.connect_to_server("127.0.0.1", port));

    aqua::grpc::ConnectResult result;
    ASSERT_TRUE(client.connect("server-round-trip", result));
    ASSERT_NE(result.session_id, 0u);
    EXPECT_EQ(result.udp_address, "127.0.0.1");
    EXPECT_EQ(result.udp_port, 50051u);
    EXPECT_EQ(result.audio_format.encoding, format.encoding);
    EXPECT_EQ(result.audio_format.channels, format.channels);
    EXPECT_EQ(result.audio_format.sample_rate, format.sample_rate);
    EXPECT_EQ(result.frame_count, 480u);

    EXPECT_TRUE(client.disconnect(result.session_id));

    server.shutdown();
    if (server_thread.joinable()) {
        server_thread.join();
    }
    EXPECT_FALSE(server.is_running());
}

TEST(GrpcServerTest, DisconnectRemovesSession)
{
    aqua::session::SessionManager sessions;
    aqua::audio::AudioFormat format;
    format.encoding = aqua::audio::AudioEncoding::PCM_F32LE;
    format.channels = 2;
    format.sample_rate = 48000;

    const auto port = find_free_tcp_port();
    aqua::grpc::GrpcServer server(
        sessions, format, 480, "127.0.0.1", port, { "127.0.0.1", 50051 });
    EXPECT_FALSE(server.is_running());

    std::thread server_thread([&server] { server.run(); });
    for (int i = 0; i < 100 && !server.is_running(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    if (!server.is_running()) {
        server.shutdown();
        if (server_thread.joinable()) {
            server_thread.join();
        }
        FAIL() << "GrpcServer did not enter running state";
    }

    aqua::grpc::GrpcClient client;
    ASSERT_TRUE(client.connect_to_server("127.0.0.1", port));

    aqua::grpc::ConnectResult result;
    ASSERT_TRUE(client.connect("disconnect-test", result));
    // Connect 创建的 session 应在 Disconnect 后被删除（SessionManager 线程安全计数）。
    EXPECT_EQ(sessions.session_count(), 1u);

    ASSERT_TRUE(client.disconnect(result.session_id));
    EXPECT_EQ(sessions.session_count(), 0u);

    server.shutdown();
    if (server_thread.joinable()) {
        server_thread.join();
    }
}

TEST(GrpcServerTest, RoundTripOverIPv6Loopback)
{
    // 先探测 IPv6 环回可用性，不可用则跳过（与 UDP 测试的 skip 策略一致）。
    {
        asio::io_context io;
        asio::ip::udp::socket probe(io);
        asio::error_code ec;
        probe.open(asio::ip::udp::v6(), ec);
        if (ec) {
            GTEST_SKIP() << "IPv6 is unavailable on this host";
        }
    }

    aqua::session::SessionManager sessions;
    aqua::audio::AudioFormat format;
    format.encoding = aqua::audio::AudioEncoding::PCM_F32LE;
    format.channels = 2;
    format.sample_rate = 48000;

    const auto port = find_free_tcp_port();
    // bind "::1" -> gRPC 监听地址被格式化为 [::1]:port（format_host_port）。
    aqua::grpc::GrpcServer server(
        sessions, format, 480, "::1", port, { "::1", 50051 });
    EXPECT_FALSE(server.is_running());

    std::thread server_thread([&server] { server.run(); });
    for (int i = 0; i < 100 && !server.is_running(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    if (!server.is_running()) {
        server.shutdown();
        if (server_thread.joinable()) {
            server_thread.join();
        }
        FAIL() << "GrpcServer did not enter running state on IPv6";
    }

    aqua::grpc::GrpcClient client;
    if (!client.connect_to_server("::1", port)) {
        server.shutdown();
        if (server_thread.joinable()) {
            server_thread.join();
        }
        GTEST_SKIP() << "gRPC client could not connect over IPv6 loopback";
    }

    aqua::grpc::ConnectResult result;
    ASSERT_TRUE(client.connect("ipv6-test", result));
    EXPECT_EQ(result.udp_address, "::1");
    EXPECT_EQ(result.udp_port, 50051u);

    EXPECT_TRUE(client.disconnect(result.session_id));

    server.shutdown();
    if (server_thread.joinable()) {
        server_thread.join();
    }
}

} // namespace
