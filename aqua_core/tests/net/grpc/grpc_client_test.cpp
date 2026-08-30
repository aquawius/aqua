#include "aqua/net/grpc/grpc_client.h"
#include "aqua/net/grpc/grpc_config.h"
#include "aqua/net/grpc/grpc_server.h"

#include <asio.hpp>
#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

class TestAudioService final : public aqua::pb::AudioService::Service {
public:
    std::string udp_address = "127.0.0.1";
    std::uint32_t udp_port = 40000;
    std::uint32_t session_id = 0x12345678u;
    bool invalid_audio_format = false;
    bool invalid_udp_address = false;
    bool invalid_udp_port = false;
    bool invalid_frame_count = false;
    std::atomic<unsigned> disconnect_calls { 0 };

    ::grpc::Status Connect(::grpc::ServerContext*, const aqua::pb::ConnectRequest*,
        aqua::pb::ConnectResponse* response) override
    {
        response->set_session_id(session_id);
        response->mutable_udp()->set_address(invalid_udp_address ? "not-an-ip" : udp_address);
        response->mutable_udp()->set_port(invalid_udp_port ? 0 : udp_port);
        if (!invalid_audio_format) {
            response->mutable_audio_format()->set_encoding(
                aqua::pb::AudioFormat::ENCODING_PCM_F32LE);
            response->mutable_audio_format()->set_channels(2);
            response->mutable_audio_format()->set_sample_rate(48000);
        }
        response->set_frame_count(invalid_frame_count ? 0 : 480);
        return ::grpc::Status::OK;
    }

    ::grpc::Status Disconnect(::grpc::ServerContext*, const aqua::pb::DisconnectRequest*,
        aqua::pb::Empty*) override
    {
        ++disconnect_calls;
        return ::grpc::Status::OK;
    }
};

struct RunningGrpcTestServer {
    explicit RunningGrpcTestServer(TestAudioService& service)
        : service(service)
    {
        ::grpc::ServerBuilder builder;
        builder.AddListeningPort("127.0.0.1:0", ::grpc::InsecureServerCredentials(), &port);
        builder.RegisterService(&service);
        server = builder.BuildAndStart();
        if (!server) {
            throw std::runtime_error("failed to start test gRPC server");
        }
        thread = std::thread([this] { server->Wait(); });
    }

    ~RunningGrpcTestServer()
    {
        if (server) {
            server->Shutdown();
        }
        if (thread.joinable()) {
            thread.join();
        }
    }

    TestAudioService& service;
    int port { 0 };
    std::unique_ptr<::grpc::Server> server;
    std::thread thread;
};

TEST(GrpcClientTest, ConnectAcceptsIPv4UdpAdvertisement)
{
    TestAudioService service;
    RunningGrpcTestServer server(service);

    aqua::grpc::GrpcClient client;
    ASSERT_TRUE(client.connect_to_server("127.0.0.1", static_cast<std::uint16_t>(server.port)));

    aqua::grpc::ConnectResult result;
    ASSERT_TRUE(client.connect("test-client", result));
    EXPECT_EQ(result.session_id, service.session_id);
    EXPECT_EQ(result.udp_address, "127.0.0.1");
    EXPECT_EQ(result.udp_port, service.udp_port);
    EXPECT_TRUE(result.audio_format.is_valid());
    EXPECT_EQ(result.audio_format.channels, 2u);
    EXPECT_EQ(result.audio_format.sample_rate, 48000u);
}

TEST(GrpcClientTest, ConnectFallsBackToGrpcServerIpForWildcardUdpAdvertisement)
{
    TestAudioService service;
    service.udp_address = "0.0.0.0";
    RunningGrpcTestServer server(service);

    aqua::grpc::GrpcClient client;
    ASSERT_TRUE(client.connect_to_server("127.0.0.1", static_cast<std::uint16_t>(server.port)));

    aqua::grpc::ConnectResult result;
    ASSERT_TRUE(client.connect("test-client", result));
    EXPECT_EQ(result.udp_address, "127.0.0.1");
    EXPECT_EQ(result.udp_port, service.udp_port);
}

TEST(GrpcClientTest, ConnectFallsBackForEmptyUdpAdvertisement)
{
    TestAudioService service;
    service.udp_address.clear();
    RunningGrpcTestServer server(service);

    aqua::grpc::GrpcClient client;
    ASSERT_TRUE(client.connect_to_server("127.0.0.1", static_cast<std::uint16_t>(server.port)));

    aqua::grpc::ConnectResult result;
    ASSERT_TRUE(client.connect("test-client", result));
    EXPECT_EQ(result.udp_address, "127.0.0.1");
}

TEST(GrpcClientTest, ConnectAcceptsIPv6UdpAdvertisement)
{
    TestAudioService service;
    service.udp_address = "2001:db8::10";
    RunningGrpcTestServer server(service);

    aqua::grpc::GrpcClient client;
    ASSERT_TRUE(client.connect_to_server("127.0.0.1", static_cast<std::uint16_t>(server.port)));

    aqua::grpc::ConnectResult result;
    ASSERT_TRUE(client.connect("test-client", result));
    EXPECT_EQ(result.udp_address, "2001:db8::10");
}

TEST(GrpcClientTest, ConnectFallsBackForInvalidUdpAdvertisement)
{
    TestAudioService service;
    service.invalid_udp_address = true;
    RunningGrpcTestServer server(service);

    aqua::grpc::GrpcClient client;
    ASSERT_TRUE(client.connect_to_server("127.0.0.1", static_cast<std::uint16_t>(server.port)));

    aqua::grpc::ConnectResult result;
    ASSERT_TRUE(client.connect("test-client", result));
    EXPECT_EQ(result.udp_address, "127.0.0.1");
    EXPECT_EQ(result.udp_port, service.udp_port);
}

TEST(GrpcClientTest, ConnectRejectsInvalidUdpPort)
{
    TestAudioService service;
    service.invalid_udp_port = true;
    RunningGrpcTestServer server(service);

    aqua::grpc::GrpcClient client;
    ASSERT_TRUE(client.connect_to_server("127.0.0.1", static_cast<std::uint16_t>(server.port)));

    aqua::grpc::ConnectResult result;
    EXPECT_FALSE(client.connect("test-client", result));
}

TEST(GrpcClientTest, ConnectRejectsInvalidAudioFormat)
{
    TestAudioService service;
    service.invalid_audio_format = true;
    RunningGrpcTestServer server(service);

    aqua::grpc::GrpcClient client;
    ASSERT_TRUE(client.connect_to_server("127.0.0.1", static_cast<std::uint16_t>(server.port)));

    aqua::grpc::ConnectResult result;
    EXPECT_FALSE(client.connect("test-client", result));
}

TEST(GrpcClientTest, ConnectRejectsZeroFramesPerSlot)
{
    TestAudioService service;
    service.invalid_frame_count = true;
    RunningGrpcTestServer server(service);

    aqua::grpc::GrpcClient client;
    ASSERT_TRUE(client.connect_to_server("127.0.0.1", static_cast<std::uint16_t>(server.port)));

    aqua::grpc::ConnectResult result;
    EXPECT_FALSE(client.connect("test-client", result));
}

TEST(GrpcClientTest, InvalidResponseRollsBackCreatedSessionAndClearsOutput)
{
    TestAudioService service;
    service.invalid_udp_port = true;
    RunningGrpcTestServer server(service);

    aqua::grpc::GrpcClient client;
    ASSERT_TRUE(client.connect_to_server("127.0.0.1", static_cast<std::uint16_t>(server.port)));

    aqua::grpc::ConnectResult result;
    result.session_id = 0xdeadbeefu;
    result.udp_address = "stale";
    result.udp_port = 12345;
    result.frame_count = 480;

    EXPECT_FALSE(client.connect("test-client", result));
    EXPECT_EQ(result.session_id, 0u);
    EXPECT_TRUE(result.udp_address.empty());
    EXPECT_EQ(result.udp_port, 0u);
    EXPECT_EQ(result.frame_count, 0u);
    EXPECT_EQ(service.disconnect_calls.load(), 1u);
}

TEST(GrpcClientTest, DisconnectCallsRpc)
{
    TestAudioService service;
    RunningGrpcTestServer server(service);

    aqua::grpc::GrpcClient client;
    ASSERT_TRUE(client.connect_to_server("127.0.0.1", static_cast<std::uint16_t>(server.port)));
    EXPECT_TRUE(client.disconnect(service.session_id));
    EXPECT_EQ(service.disconnect_calls.load(), 1u);
}

TEST(GrpcClientTest, RejectsZeroRpcPort)
{
    aqua::grpc::GrpcClient client;
    EXPECT_FALSE(client.connect_to_server("127.0.0.1", 0));
}

TEST(GrpcClientTest, ConnectToServerRejectsInvalidAddress)
{
    aqua::grpc::GrpcClient client;
    // 非 IP 字面量：format_host_port -> parse_ip_address 抛异常，应被转为失败。
    EXPECT_FALSE(client.connect_to_server("not-an-ip", 50051));
}

TEST(GrpcClientTest, FailedReconnectClearsPreviousConnection)
{
    TestAudioService service;
    RunningGrpcTestServer server(service);

    aqua::grpc::GrpcClient client;
    ASSERT_TRUE(client.connect_to_server("127.0.0.1", static_cast<std::uint16_t>(server.port)));

    // A subsequent failed connection attempt must not leave the old channel/fallback IP usable.
    EXPECT_FALSE(client.connect_to_server("not-an-ip", 50051));
    aqua::grpc::ConnectResult result;
    EXPECT_FALSE(client.connect("test-client", result));
}

TEST(GrpcClientTest, ConnectToServerFailsForUnreachablePort)
{
    // 取一个当前空闲的 TCP 端口：连接已关闭的本地端口会立刻收到 RST，
    // WaitForConnected 快速失败，不会真的等满 GRPC_CONNECT_DEADLINE。
    asio::io_context io;
    asio::ip::tcp::acceptor acceptor(io,
        asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    const auto port = acceptor.local_endpoint().port();
    acceptor.close(); // 立即释放端口

    aqua::grpc::GrpcClient client;
    EXPECT_FALSE(client.connect_to_server("127.0.0.1", port));
}

} // namespace
