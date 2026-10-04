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
    // true = 只填 udp6 槽（模拟"只服务 IPv6 数据面"的 server：udp 槽不写 ⇒ has_udp()==false）。
    bool udp6_only = false;
    std::string udp6_address = "::1";
    std::uint32_t udp6_port = 40000;
    std::atomic<unsigned> disconnect_calls { 0 };

    ::grpc::Status Connect(::grpc::ServerContext*, const aqua::pb::ConnectRequest*,
        aqua::pb::ConnectResponse* response) override
    {
        response->set_session_id(session_id);
        if (udp6_only) {
            response->mutable_udp6()->set_address(udp6_address);
            response->mutable_udp6()->set_port(udp6_port);
        } else {
            response->mutable_udp()->set_address(invalid_udp_address ? "not-an-ip" : udp_address);
            response->mutable_udp()->set_port(invalid_udp_port ? 0 : udp_port);
        }
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
        thread = std::jthread([this] { server->Wait(); });
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
    std::jthread thread;
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
    EXPECT_EQ(result.advertised_udp_address, "127.0.0.1");
    EXPECT_EQ(result.advertised_udp_port, service.udp_port);
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
    EXPECT_EQ(result.advertised_udp_address, "127.0.0.1");
    EXPECT_EQ(result.advertised_udp_port, service.udp_port);
    // 通配槽 = sentinel：客户端改用自己连控制面时的地址，并如实标记。
    EXPECT_TRUE(result.udp_address_from_sentinel);
    EXPECT_EQ(result.udp_family, aqua::net::IpFamily::V4);
}

// 空地址不再是"静默回落到 gRPC 地址"：空串无法与 sentinel 区分，属于**无效槽**，
// 必须响亮失败（否则将来有人改用空串表示空槽就会静默退化成"发向自己"）。
TEST(GrpcClientTest, ConnectRejectsEmptyUdpAdvertisement)
{
    TestAudioService service;
    service.udp_address.clear();
    RunningGrpcTestServer server(service);

    aqua::grpc::GrpcClient client;
    ASSERT_TRUE(client.connect_to_server("127.0.0.1", static_cast<std::uint16_t>(server.port)));

    aqua::grpc::ConnectResult result;
    EXPECT_FALSE(client.connect("test-client", result));
}

// 客户端自证：v4 客户端的槽里出现 v6 地址 ⇒ 硬失败（不做跨族回退，
// 否则只会把故障推迟成"连上了但收不到音频"）。
TEST(GrpcClientTest, ConnectRejectsCrossFamilyUdpAdvertisement)
{
    TestAudioService service;
    service.udp_address = "2001:db8::10";
    RunningGrpcTestServer server(service);

    aqua::grpc::GrpcClient client;
    ASSERT_TRUE(client.connect_to_server("127.0.0.1", static_cast<std::uint16_t>(server.port)));

    aqua::grpc::ConnectResult result;
    EXPECT_FALSE(client.connect("test-client", result));
}

// 空槽（服务端只服务 IPv6，udp 槽不存在）而客户端是 IPv4 ⇒ 连接期硬失败，
// 并且已创建的 session 被回滚。
TEST(GrpcClientTest, ConnectRejectsMissingSlotForClientFamily)
{
    TestAudioService service;
    service.udp6_only = true;
    RunningGrpcTestServer server(service);

    aqua::grpc::GrpcClient client;
    ASSERT_TRUE(client.connect_to_server("127.0.0.1", static_cast<std::uint16_t>(server.port)));

    aqua::grpc::ConnectResult result;
    EXPECT_FALSE(client.connect("test-client", result));
    EXPECT_EQ(service.disconnect_calls.load(), 1u);
}

// v4-mapped 字面量（::ffff:a.b.c.d）没有自洽的族归宿：归一化后算 v4，但文本形态会让
// UdpTransport 开出 AF_INET6 socket。客户端拨号目标与槽内地址都必须拒绝它。
TEST(GrpcClientTest, RejectsV4MappedServerAddress)
{
    aqua::grpc::GrpcClient client;
    EXPECT_FALSE(client.connect_to_server("::ffff:127.0.0.1", 50051));
}

TEST(GrpcClientTest, ConnectRejectsV4MappedUdpAdvertisement)
{
    TestAudioService service;
    service.udp_address = "::ffff:127.0.0.1";
    RunningGrpcTestServer server(service);

    aqua::grpc::GrpcClient client;
    ASSERT_TRUE(client.connect_to_server("127.0.0.1", static_cast<std::uint16_t>(server.port)));

    aqua::grpc::ConnectResult result;
    EXPECT_FALSE(client.connect("test-client", result));
}

TEST(GrpcClientTest, ConnectRejectsInvalidUdpAdvertisement)
{
    TestAudioService service;
    service.invalid_udp_address = true;
    RunningGrpcTestServer server(service);

    aqua::grpc::GrpcClient client;
    ASSERT_TRUE(client.connect_to_server("127.0.0.1", static_cast<std::uint16_t>(server.port)));

    aqua::grpc::ConnectResult result;
    EXPECT_FALSE(client.connect("test-client", result));
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
    result.advertised_udp_address = "stale";
    result.advertised_udp_port = 12345;
    result.frame_count = 480;

    EXPECT_FALSE(client.connect("test-client", result));
    EXPECT_EQ(result.session_id, 0u);
    EXPECT_TRUE(result.advertised_udp_address.empty());
    EXPECT_EQ(result.advertised_udp_port, 0u);
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
