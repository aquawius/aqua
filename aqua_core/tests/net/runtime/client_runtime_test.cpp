#include "aqua/runtime/client_runtime.h"

#include "aqua/net/udp/network_frame.h"

#include <gtest/gtest.h>
#include <asio.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <thread>
#include <vector>

namespace {

using aqua::runtime::ClientRuntime;
using aqua::runtime::ClientRuntimeConfig;

constexpr std::uint32_t kFrameBytes = 4; // PCM_F32LE 单声道

aqua::audio::AudioFormat make_format()
{
    return aqua::audio::AudioFormat { aqua::audio::AudioEncoding::PCM_F32LE, 1, 48000 };
}

ClientRuntimeConfig make_config()
{
    ClientRuntimeConfig cfg;
    cfg.jitter_buffer_slots = 10; // target 60% = 6 槽，便于测试
    return cfg;
}

std::vector<std::byte> make_payload(std::uint32_t frames, std::uint8_t fill)
{
    std::vector<std::byte> d(static_cast<std::size_t>(frames) * kFrameBytes);
    std::fill(d.begin(), d.end(), static_cast<std::byte>(fill));
    return d;
}

TEST(ClientRuntimeTest, RoutesAudioDatagramToJitterBuffer)
{
    asio::io_context ioc;
    auto rt = std::make_shared<ClientRuntime>(ioc, make_config());
    ASSERT_TRUE(rt->setup_playback(make_format(), 4));

    const auto dgram = aqua::net::NetworkFrame::audio(100, make_payload(4, 42)).encode();
    rt->handle_datagram(asio::ip::udp::endpoint {}, dgram);

    ASSERT_NE(rt->jitter_buffer(), nullptr);
    EXPECT_EQ(rt->jitter_buffer()->used_slots(), 1u);
}

TEST(ClientRuntimeTest, IgnoresNonAudioDatagram)
{
    asio::io_context ioc;
    auto rt = std::make_shared<ClientRuntime>(ioc, make_config());
    ASSERT_TRUE(rt->setup_playback(make_format(), 4));

    const auto hello = aqua::net::NetworkFrame::hello(0x12345678u).encode();
    rt->handle_datagram(asio::ip::udp::endpoint {}, hello);
    EXPECT_EQ(rt->jitter_buffer()->used_slots(), 0u);
}

TEST(ClientRuntimeTest, PullPlaybackReturnsBufferedFrames)
{
    asio::io_context ioc;
    auto rt = std::make_shared<ClientRuntime>(ioc, make_config());
    ASSERT_TRUE(rt->setup_playback(make_format(), 4));

    // 推 6 帧（lead=6=target，N=10）→ 锚定后 pull 出 seq 100（fill=101）。
    for (std::uint64_t s = 100; s <= 105; ++s) {
        const auto dgram = aqua::net::NetworkFrame::audio(
            s, make_payload(4, static_cast<std::uint8_t>(s + 1))).encode();
        rt->handle_datagram(asio::ip::udp::endpoint {}, dgram);
    }

    std::vector<std::byte> out(4 * kFrameBytes);
    EXPECT_EQ(rt->pull_playback(out), 4u);
    EXPECT_EQ(std::to_integer<std::uint8_t>(out[0]), 101u);
}

} // namespace

namespace {

// connect 失败清理 session 需要真实 gRPC 服务器（Connect 建 session，Disconnect 计数）。
class ConnectTestService final : public aqua::pb::AudioService::Service {
public:
    std::atomic<unsigned> disconnect_calls { 0 };

    ::grpc::Status Connect(::grpc::ServerContext*, const aqua::pb::ConnectRequest*,
        aqua::pb::ConnectResponse* response) override
    {
        response->set_session_id(0x12345678u);
        response->mutable_udp()->set_address("127.0.0.1");
        response->mutable_udp()->set_port(40000);
        response->mutable_audio_format()->set_encoding(
            aqua::pb::AudioFormat::ENCODING_PCM_F32LE);
        response->mutable_audio_format()->set_channels(1);
        response->mutable_audio_format()->set_sample_rate(48000);
        response->set_frames_per_slot(4);
        return ::grpc::Status::OK;
    }

    ::grpc::Status Disconnect(::grpc::ServerContext*, const aqua::pb::DisconnectRequest*,
        aqua::pb::Empty*) override
    {
        disconnect_calls.fetch_add(1, std::memory_order_relaxed);
        return ::grpc::Status::OK;
    }
};

struct RunningGrpcServer {
    explicit RunningGrpcServer(ConnectTestService& svc)
        : service(svc)
    {
        ::grpc::ServerBuilder builder;
        builder.AddListeningPort("127.0.0.1:0", ::grpc::InsecureServerCredentials(), &port);
        builder.RegisterService(&service);
        server = builder.BuildAndStart();
        thread = std::thread([this] { server->Wait(); });
    }

    ~RunningGrpcServer()
    {
        if (server) {
            server->Shutdown();
        }
        if (thread.joinable()) {
            thread.join();
        }
    }

    ConnectTestService& service;
    int port { 0 };
    std::unique_ptr<::grpc::Server> server;
    std::thread thread;
};

TEST(ClientRuntimeTest, ConnectFailureCleansUpSession)
{
    ConnectTestService service;
    RunningGrpcServer server(service);

    asio::io_context ioc;
    aqua::runtime::ClientRuntimeConfig cfg;
    cfg.jitter_buffer_slots = 0; // 使 setup_playback（JB create）失败
    auto rt = std::make_shared<aqua::runtime::ClientRuntime>(ioc, cfg);

    EXPECT_FALSE(rt->connect("127.0.0.1", static_cast<std::uint16_t>(server.port), "test"));
    EXPECT_EQ(service.disconnect_calls.load(), 1u);
}

} // namespace
