#include "aqua/net/grpc/grpc_client.h"
#include "aqua/net/grpc/grpc_server.h"

#include <asio.hpp>
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <future>
#include <memory>
#include <string>
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

    // 端口 0 让 OS 自动选择一个空闲的 gRPC 监听端口。这里只验证启动/关闭生命周期，
    // 因为 GrpcServer 有意不暴露它实际选中的端口。
    aqua::grpc::GrpcServer server(
        sessions, format, 480, "127.0.0.1", 0, { "127.0.0.1", 9999 });

    // shutdown 设计为任意线程安全，且允许在 run() 之前调用。
    server.shutdown();
    EXPECT_FALSE(server.is_running());
}

} // namespace

namespace {

std::uint16_t find_free_tcp_port()
{
    asio::io_context io;
    asio::ip::tcp::acceptor acceptor(io, asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
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
    EXPECT_EQ(result.advertised_udp_address, "127.0.0.1");
    EXPECT_EQ(result.advertised_udp_port, 50051u);
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

TEST(GrpcServerTest, WildcardUdpAdvertisementFallsBackToControlPlaneAddress)
{
    aqua::session::SessionManager sessions;
    aqua::audio::AudioFormat format;
    format.encoding = aqua::audio::AudioEncoding::PCM_F32LE;
    format.channels = 2;
    format.sample_rate = 48000;

    const auto port = find_free_tcp_port();
    aqua::grpc::GrpcServer server(
        sessions, format, 480, "127.0.0.1", port, { "0.0.0.0", 50051 });

    std::thread server_thread([&server] { server.run(); });
    for (int i = 0; i < 100 && !server.is_running(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    ASSERT_TRUE(server.is_running());

    aqua::grpc::GrpcClient client;
    ASSERT_TRUE(client.connect_to_server("127.0.0.1", port));

    aqua::grpc::ConnectResult result;
    ASSERT_TRUE(client.connect("wildcard-advertise-test", result));
    EXPECT_EQ(result.advertised_udp_address, "127.0.0.1");
    EXPECT_EQ(result.advertised_udp_port, 50051u);

    EXPECT_TRUE(client.disconnect(result.session_id));
    server.shutdown();
    if (server_thread.joinable()) {
        server_thread.join();
    }
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
    EXPECT_EQ(result.advertised_udp_address, "::1");
    EXPECT_EQ(result.advertised_udp_port, 50051u);

    EXPECT_TRUE(client.disconnect(result.session_id));

    server.shutdown();
    if (server_thread.joinable()) {
        server_thread.join();
    }
}

TEST(GrpcServerServiceTest, RejectsEmptyAndOverlongClientName)
{
    aqua::session::SessionManager sessions;
    aqua::audio::AudioFormat format;
    format.encoding = aqua::audio::AudioEncoding::PCM_F32LE;
    format.channels = 2;
    format.sample_rate = 48000;
    aqua::grpc::GrpcServerService service(sessions, format, 480, { "127.0.0.1", 9999 });

    aqua::pb::ConnectRequest empty_request;
    aqua::pb::ConnectResponse response;
    auto status = service.Connect(nullptr, &empty_request, &response);
    EXPECT_EQ(status.error_code(), ::grpc::StatusCode::INVALID_ARGUMENT);

    aqua::pb::ConnectRequest long_request;
    long_request.set_client_name(std::string(129, 'x'));
    response.Clear();
    status = service.Connect(nullptr, &long_request, &response);
    EXPECT_EQ(status.error_code(), ::grpc::StatusCode::INVALID_ARGUMENT);
    EXPECT_EQ(sessions.session_count(), 0u);
}

} // namespace

namespace {

// 启动带 run 线程的测试 server，返回其 gRPC 端口（调用方负责 shutdown + join）。
struct TestServer {
    aqua::session::SessionManager sessions;
    std::unique_ptr<aqua::grpc::GrpcServer> server;
    std::thread thread;
    std::uint16_t port = 0;

    bool start()
    {
        aqua::audio::AudioFormat format;
        format.encoding = aqua::audio::AudioEncoding::PCM_F32LE;
        format.channels = 2;
        format.sample_rate = 48000;
        port = find_free_tcp_port();
        server = std::make_unique<aqua::grpc::GrpcServer>(sessions, format, 480,
            "127.0.0.1", port, aqua::grpc::AdvertisedUdpEndpoint { "127.0.0.1", 50051 });
        thread = std::thread([this] { server->run(); });
        for (int i = 0; i < 100 && !server->is_running(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return server->is_running();
    }

    void stop()
    {
        if (server) {
            server->shutdown();
        }
        if (thread.joinable()) {
            thread.join();
        }
    }
};

TEST(GrpcKeepaliveTest, ServiceValidatesSession)
{
    // service 直调（不需要 ping 线程）：存在 session → valid=true，
    // 不存在 → valid=false。session 存活语义的最小单元测试。
    aqua::session::SessionManager sessions;
    aqua::audio::AudioFormat format;
    format.encoding = aqua::audio::AudioEncoding::PCM_F32LE;
    format.channels = 2;
    format.sample_rate = 48000;
    aqua::grpc::GrpcServerService service(sessions, format, 480, { "127.0.0.1", 9999 });

    aqua::pb::KeepaliveRequest req;
    aqua::pb::KeepaliveResponse resp;
    req.set_session_id(0x1234u);
    EXPECT_TRUE(service.Keepalive(nullptr, &req, &resp).ok());
    EXPECT_FALSE(resp.session_valid());

    const auto id = sessions.create_session();
    ASSERT_TRUE(id.has_value());
    req.set_session_id(*id);
    EXPECT_TRUE(service.Keepalive(nullptr, &req, &resp).ok());
    EXPECT_TRUE(resp.session_valid());
}

TEST(GrpcKeepaliveTest, DeadServerTriggersHandler)
{
    TestServer ts;
    ASSERT_TRUE(ts.start());

    aqua::grpc::GrpcClient client;
    ASSERT_TRUE(client.connect_to_server("127.0.0.1", ts.port));
    aqua::grpc::ConnectResult result;
    ASSERT_TRUE(client.connect("keepalive-test", result));

    // server 停服后，ping 线程应在连续 GRPC_KEEPALIVE_MISS_THRESHOLD 次失败后
    // 判定 TransportDead（单次抖动不判死，与 UDP 稳态对称）。
    // 用短 interval 加速（生产用 GRPC_KEEPALIVE_INTERVAL）。
    std::promise<aqua::grpc::GrpcClient::KeepaliveStatus> fired;
    auto future = fired.get_future();
    client.start_keepalive(result.session_id, std::chrono::milliseconds(100),
        [&fired](aqua::grpc::GrpcClient::KeepaliveStatus status) noexcept {
            try {
                fired.set_value(status);
            } catch (...) {
            }
        });

    ts.stop();
    // 阈值未满时不触发：250ms 内约 2 个周期（阈值 5），必须无回调。
    EXPECT_EQ(future.wait_for(std::chrono::milliseconds(250)), std::future_status::timeout);
    ASSERT_EQ(future.wait_for(std::chrono::seconds(10)), std::future_status::ready);
    EXPECT_EQ(future.get(), aqua::grpc::GrpcClient::KeepaliveStatus::TransportDead);
}

TEST(GrpcKeepaliveTest, GoneSessionTriggersHandler)
{
    TestServer ts;
    ASSERT_TRUE(ts.start());

    aqua::grpc::GrpcClient client;
    ASSERT_TRUE(client.connect_to_server("127.0.0.1", ts.port));
    aqua::grpc::ConnectResult result;
    ASSERT_TRUE(client.connect("keepalive-test", result));

    // server 端删掉 session（模拟 reaper 清理）：下一次 ping 返回 valid=false。
    EXPECT_TRUE(ts.sessions.remove_session(result.session_id));

    std::promise<aqua::grpc::GrpcClient::KeepaliveStatus> fired;
    auto future = fired.get_future();
    client.start_keepalive(result.session_id, std::chrono::milliseconds(100),
        [&fired](aqua::grpc::GrpcClient::KeepaliveStatus status) noexcept {
            try {
                fired.set_value(status);
            } catch (...) {
            }
        });
    ASSERT_EQ(future.wait_for(std::chrono::seconds(10)), std::future_status::ready);
    EXPECT_EQ(future.get(), aqua::grpc::GrpcClient::KeepaliveStatus::SessionGone);

    ts.stop();
}

TEST(GrpcKeepaliveTest, StopKeepaliveIsSafe)
{
    TestServer ts;
    ASSERT_TRUE(ts.start());

    aqua::grpc::GrpcClient client;
    ASSERT_TRUE(client.connect_to_server("127.0.0.1", ts.port));
    aqua::grpc::ConnectResult result;
    ASSERT_TRUE(client.connect("keepalive-test", result));

    std::atomic<int> calls { 0 };
    client.start_keepalive(result.session_id, std::chrono::milliseconds(100),
        [&calls](aqua::grpc::GrpcClient::KeepaliveStatus) noexcept {
            calls.fetch_add(1, std::memory_order_relaxed);
        });
    client.stop_keepalive();
    client.stop_keepalive(); // 幂等：重复调用安全
    // 健康 server + 立即停止：不断言回调（只要求 stop 不挂死）。
    SUCCEED();

    ts.stop();
}

} // namespace
