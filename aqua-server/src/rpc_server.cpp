//
// Created by aquawius on 25-1-20.
//

#include "rpc_server.h"
#include "network_server.h"
#include "session_manager.h"
#include "audio_manager.h"

#include <boost/asio.hpp>
#include <boost/uuid/random_generator.hpp>
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <spdlog/spdlog.h>

RPCServer::RPCServer(network_server& manager, std::shared_ptr<audio_manager> audio_mgr)
    : m_network_manager(manager)
      , m_audio_manager(audio_mgr) {}

RPCServer::~RPCServer()
{
    session_manager::get_instance().clear_all_sessions();
}

grpc::Status RPCServer::Connect(grpc::ServerContext* context,
    const ConnectRequest* request,
    ConnectResponse* response)
{
    // Request info
    const std::string& client_addr = request->client_address();
    const auto client_port = static_cast<uint16_t>(request->client_port());

    spdlog::info("[rpc_server] Connect request, address={}:{}", client_addr, client_port);

    boost::system::error_code ec;
    auto address = boost::asio::ip::make_address(client_addr, ec);
    if (ec) {
        spdlog::error("[rpc_server] Invalid IP address format: {}", client_addr);
        response->set_success(false);
        response->set_error_message("Invalid IP address");
        return grpc::Status { grpc::StatusCode::INVALID_ARGUMENT, "Invalid IP address" };
    }

    if (!m_audio_manager) {
        return grpc::Status(grpc::StatusCode::UNAVAILABLE, "Audio manager is not available");
    }

    // 获取服务器首选音频格式
    const auto& server_format = m_audio_manager->get_preferred_format();

    // 创建音频格式响应
    auto* negotiated_format = response->mutable_server_format();

    // 设置服务器音频格式参数
    negotiated_format->set_channels(server_format.channels);
    negotiated_format->set_sample_rate(server_format.sample_rate);
    negotiated_format->set_encoding(convert_encoding_to_proto(server_format.encoding));

    spdlog::info("[rpc_server] Using server's audio format: {} Hz, {} channels, encoding: {}",
        negotiated_format->sample_rate(),
        negotiated_format->channels(),
        static_cast<int>(negotiated_format->encoding()));

    // success
    boost::asio::ip::udp::endpoint endpoint(address, client_port);
    boost::uuids::uuid uuid = boost::uuids::random_generator()();
    std::string client_uuid_str = boost::uuids::to_string(uuid);
    spdlog::info("[rpc_server] Generated client UUID: {}", client_uuid_str);

    if (!session_manager::get_instance().add_session(client_uuid_str, endpoint)) {
        response->set_success(false);
        response->set_error_message("Endpoint already in use");
        return grpc::Status { grpc::StatusCode::ALREADY_EXISTS, "Endpoint already in use" };
    }

    response->set_success(true);
    response->set_error_message("OK");
    response->set_client_uuid(client_uuid_str);
    response->set_server_address(m_network_manager.get_server_address());
    response->set_server_port(m_network_manager.get_server_udp_port());

    return grpc::Status::OK;
}

grpc::Status RPCServer::Disconnect(grpc::ServerContext* context,
    const DisconnectRequest* request,
    DisconnectResponse* response)
{
    const std::string& client_uuid = request->client_uuid();
    spdlog::info("[rpc_server] Disconnect request for client_uuid={}", client_uuid);

    bool result = session_manager::get_instance().remove_session(client_uuid);
    response->set_success(result);
    return grpc::Status::OK;
}

grpc::Status RPCServer::KeepAlive(grpc::ServerContext* context,
    const KeepAliveRequest* request,
    KeepAliveResponse* response)
{
    const std::string& client_uuid = request->client_uuid();
    spdlog::debug("[rpc_server] KeepAlive request for client_uuid={}", client_uuid);

    bool success = session_manager::get_instance().update_keepalive(client_uuid);
    if (!success) {
        response->set_success(false);
        response->set_error_message("Session not found or expired");
        return grpc::Status { grpc::StatusCode::NOT_FOUND, "Session not found or expired" };
    }
    response->set_success(true);
    response->set_error_message("OK");
    return grpc::Status::OK;
}

grpc::Status RPCServer::GetAudioFormat(grpc::ServerContext* context,
    const GetAudioFormatRequest* request,
    AudioFormatResponse* response)
{
    const std::string& client_uuid = request->client_uuid();
    spdlog::debug("[rpc_server] GetAudioFormat request for client_uuid={}", client_uuid);

    // 验证客户端会话是否存在
    if (!session_manager::get_instance().is_session_valid(client_uuid)) {
        response->set_error_message("Session not found or expired");
        return grpc::Status { grpc::StatusCode::NOT_FOUND, "Session not found or expired" };
    }

    if (!m_audio_manager) {
        return grpc::Status(grpc::StatusCode::UNAVAILABLE, "Audio manager is not available");
    }

    // 获取服务器当前首选音频格式
    const auto& server_format = m_audio_manager->get_preferred_format();

    // 创建音频格式响应
    auto* format = response->mutable_format();

    // 设置服务器音频格式
    format->set_channels(server_format.channels);
    format->set_sample_rate(server_format.sample_rate);

    // 设置编码格式（直接转换，避免双重转换）
    auto proto_encoding = convert_encoding_to_proto(server_format.encoding);
    format->set_encoding(proto_encoding);

    spdlog::debug("[rpc_server] Responded with audio format: {} Hz, {} channels, encoding: {}",
        format->sample_rate(),
        format->channels(),
        static_cast<int>(format->encoding()));

    return grpc::Status::OK;
}