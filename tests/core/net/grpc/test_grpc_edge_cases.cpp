#include "core/audio/audio_config.h"
#include "core/audio/audio_format.h"
#include "core/net/grpc/grpc_audio_format_converter.h"
#include "core/net/grpc/grpc_client.h"
#include "core/net/grpc/grpc_include.h"
#include "core/net/grpc/grpc_server.h"
#include "core/net/address/address_utils.h"

#include <gtest/gtest.h>
#include <asio.hpp>

#include <chrono>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <thread>

namespace {

std::uint16_t find_free_tcp_port(std::string host = "127.0.0.1")
{
    asio::io_context io;
    asio::ip::tcp::acceptor acceptor(io, asio::ip::tcp::endpoint(
        aqua::net::parse_ip_address(host), 0));
    return acceptor.local_endpoint().port();
}

class MalformedConnectService final : public aqua::pb::AudioService::Service {
public:
    enum class Mode {
        EmptyAddress,
        InvalidAddress,
        ZeroPort,
        TooLargePort,
        InvalidFormat,
    };

    explicit MalformedConnectService(Mode mode)
        : mode_(mode)
    {
    }

    ::grpc::Status Connect(::grpc::ServerContext*, const aqua::pb::ConnectRequest*,
        aqua::pb::ConnectResponse* response) override
    {
        response->set_session_id(1);
        switch (mode_) {
        case Mode::EmptyAddress:
            response->mutable_udp()->set_address("");
            response->mutable_udp()->set_port(9999);
            break;
        case Mode::InvalidAddress:
            response->mutable_udp()->set_address("not-an-ip");
            response->mutable_udp()->set_port(9999);
            break;
        case Mode::ZeroPort:
            response->mutable_udp()->set_address("127.0.0.1");
            response->mutable_udp()->set_port(0);
            break;
        case Mode::TooLargePort:
            response->mutable_udp()->set_address("127.0.0.1");
            response->mutable_udp()->set_port(65536);
            break;
        case Mode::InvalidFormat:
            response->mutable_udp()->set_address("127.0.0.1");
            response->mutable_udp()->set_port(9999);
            response->mutable_audio_format()->set_encoding(
                aqua::pb::AudioFormat::ENCODING_INVALID);
            response->mutable_audio_format()->set_channels(2);
            response->mutable_audio_format()->set_sample_rate(48000);
            break;
        }
        return ::grpc::Status::OK;
    }

    ::grpc::Status Disconnect(::grpc::ServerContext*, const aqua::pb::DisconnectRequest*,
        aqua::pb::Empty*) override
    {
        return ::grpc::Status::OK;
    }

private:
    Mode mode_;
};

struct RawGrpcServer {
    RawGrpcServer(MalformedConnectService& service, std::string bind_ip)
    {
        port = find_free_tcp_port(bind_ip);
        address = aqua::net::format_host_port(bind_ip, port);

        ::grpc::ServerBuilder builder;
        builder.AddListeningPort(address, ::grpc::InsecureServerCredentials());
        builder.RegisterService(&service);
        server = builder.BuildAndStart();
        if (!server) {
            throw std::runtime_error("failed to start raw gRPC test server");
        }
        thread = std::thread([this] { server->Wait(); });
    }

    ~RawGrpcServer()
    {
        if (server) {
            server->Shutdown();
        }
        if (thread.joinable()) {
            thread.join();
        }
    }

    std::string address;
    std::uint16_t port = 0;
    std::unique_ptr<::grpc::Server> server;
    std::thread thread;
};

TEST(GrpcEdgeTest, ClientWithoutChannelRejectsConnectAndDisconnect)
{
    aqua::grpc::GrpcClient client;
    aqua::grpc::ConnectResult result;
    EXPECT_FALSE(client.connect("no-server", result));
    EXPECT_FALSE(client.disconnect(1));
}

TEST(GrpcEdgeTest, ServerShutdownIsIdempotent)
{
    aqua::SessionManager sessions;
    aqua::audio::AudioFormat format;
    format.encoding = aqua::audio::AudioEncoding::PCM_F32LE;
    format.channels = 2;
    format.sample_rate = 48000;

    const auto port = find_free_tcp_port();
    aqua::grpc::GrpcServer server(
        sessions, format, "127.0.0.1", port, "127.0.0.1", 50051);

    std::thread thread([&server] { server.run(); });
    for (int i = 0; i < 100 && !server.is_running(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    ASSERT_TRUE(server.is_running());

    server.shutdown();
    server.shutdown();
    thread.join();
    EXPECT_FALSE(server.is_running());
}

TEST(GrpcEdgeTest, InvalidServerCannotEnterRunLoop)
{
    aqua::SessionManager sessions;
    aqua::audio::AudioFormat format;
    format.encoding = aqua::audio::AudioEncoding::PCM_F32LE;
    format.channels = 2;
    format.sample_rate = 48000;

    aqua::grpc::GrpcServer server(
        sessions, format, "not-an-ip", 50051, "127.0.0.1", 9999);
    EXPECT_FALSE(server.is_running());
    server.run();
    EXPECT_FALSE(server.is_running());
}

TEST(GrpcEdgeTest, ConverterAcceptsMinimumValidBounds)
{
    aqua::pb::AudioFormat proto;
    proto.set_encoding(aqua::pb::AudioFormat::ENCODING_PCM_F32LE);
    proto.set_channels(1);
    proto.set_sample_rate(1);

    const auto fmt = aqua::from_proto(proto);
    EXPECT_TRUE(fmt.is_valid());
    EXPECT_EQ(fmt.channels, 1u);
    EXPECT_EQ(fmt.sample_rate, 1u);
}

TEST(GrpcEdgeTest, ConverterAcceptsMaximumValidBounds)
{
    aqua::pb::AudioFormat proto;
    proto.set_encoding(aqua::pb::AudioFormat::ENCODING_PCM_F32LE);
    proto.set_channels(static_cast<std::int32_t>(aqua::audio::AUDIO_FORMAT_MAX_CHANNELS));
    proto.set_sample_rate(static_cast<std::int32_t>(aqua::audio::AUDIO_FORMAT_MAX_SAMPLE_RATE));

    const auto fmt = aqua::from_proto(proto);
    EXPECT_TRUE(fmt.is_valid());
    EXPECT_EQ(fmt.channels, aqua::audio::AUDIO_FORMAT_MAX_CHANNELS);
    EXPECT_EQ(fmt.sample_rate, aqua::audio::AUDIO_FORMAT_MAX_SAMPLE_RATE);
}

TEST(GrpcEdgeTest, ClientRejectsEmptyAdvertisedUdpAddress)
{
    MalformedConnectService service(MalformedConnectService::Mode::EmptyAddress);
    RawGrpcServer server(service, "127.0.0.1");

    aqua::grpc::GrpcClient client;
    ASSERT_TRUE(client.connect_to_server("127.0.0.1", server.port));

    aqua::grpc::ConnectResult result;
    EXPECT_FALSE(client.connect("bad-address", result));
}

TEST(GrpcEdgeTest, ClientRejectsInvalidAdvertisedUdpAddress)
{
    MalformedConnectService service(MalformedConnectService::Mode::InvalidAddress);
    RawGrpcServer server(service, "127.0.0.1");
    aqua::grpc::GrpcClient client;
    ASSERT_TRUE(client.connect_to_server("127.0.0.1", server.port));

    aqua::grpc::ConnectResult result;
    EXPECT_FALSE(client.connect("invalid-address", result));
}

TEST(GrpcEdgeTest, ClientRejectsZeroAdvertisedUdpPort)
{
    MalformedConnectService service(MalformedConnectService::Mode::ZeroPort);
    RawGrpcServer server(service, "127.0.0.1");
    aqua::grpc::GrpcClient client;
    ASSERT_TRUE(client.connect_to_server("127.0.0.1", server.port));

    aqua::grpc::ConnectResult result;
    EXPECT_FALSE(client.connect("zero-port", result));
}

TEST(GrpcEdgeTest, ClientRejectsOverflowingAdvertisedUdpPort)
{
    MalformedConnectService service(MalformedConnectService::Mode::TooLargePort);
    RawGrpcServer server(service, "127.0.0.1");
    aqua::grpc::GrpcClient client;
    ASSERT_TRUE(client.connect_to_server("127.0.0.1", server.port));

    aqua::grpc::ConnectResult result;
    EXPECT_FALSE(client.connect("large-port", result));
}

TEST(GrpcEdgeTest, ClientRejectsInvalidAdvertisedAudioFormat)
{
    MalformedConnectService service(MalformedConnectService::Mode::InvalidFormat);
    RawGrpcServer server(service, "127.0.0.1");
    aqua::grpc::GrpcClient client;
    ASSERT_TRUE(client.connect_to_server("127.0.0.1", server.port));

    aqua::grpc::ConnectResult result;
    EXPECT_FALSE(client.connect("invalid-format", result));
}

} // namespace
