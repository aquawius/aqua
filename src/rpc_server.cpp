//
// Created by aquawius on 25-1-20.
//

#include "rpc_server.h"
#include "network_server.h"
#include "session_manager.h"
#include <boost/asio.hpp>
#include <spdlog/spdlog.h>

RPCServer::RPCServer(network_server& manager)
    : m_network_manager(manager)
{
}

grpc::Status RPCServer::Connect(grpc::ServerContext* context,
    const ConnectRequest* request, ConnectResponse* response)
{
    const std::string& client_id = request->client_id();
    const std::string& client_addr = request->client_address();
    const uint16_t client_port = static_cast<uint16_t>(request->client_port());

    spdlog::info("[RPCServer] Connect request from client_id={}, address={}:{}", client_id, client_addr, client_port); // 尝试将 client_address 转为 boost::asio 可用的地址
    boost::system::error_code ec;
    auto address = boost::asio::ip::make_address(client_addr, ec);
    if (ec) { // IP 地址无效时，设置对客户端的响应（success = false, error_message 用于说明）
        spdlog::error("[RPCServer] Invalid IP address format: {}", client_addr);
        response->set_success(false);
        response->set_error_message("Invalid IP address"); // 其余字段保持默认即可
        return grpc::Status { grpc::StatusCode::INVALID_ARGUMENT, "Invalid IP address" };
    }
    boost::asio::ip::udp::endpoint endpoint(address, client_port); // 调用 session_manager 添加或更新 session

    bool is_new = session_manager::get_instance().add_session(client_id, endpoint);
    if (is_new) {
        spdlog::info("[RPCServer] Created new session for {}", client_id);
    } else {
        spdlog::info("[RPCServer] Replaced existing session for {}", client_id);
    }
    // 构造返回值（示例：将本服务器的“地址与端口”告知客户端）
    // 下方示例中，使用 manager_.get_default_address() 作为 server_address
    // server_port 可根据实际情况返回你用到的 UDP 或 gRPC 端口
    response->set_success(true);
    response->set_error_message(""); // 没有错误
    response->set_server_address(m_network_manager.get_default_address()); // 假设 gRPC 与 UDP 端口相同
    response->set_server_port(10120);
    return grpc::Status::OK;
}

grpc::Status RPCServer::Disconnect(grpc::ServerContext* context,
    const DisconnectRequest* request, DisconnectResponse* response)
{
    const std::string& client_id = request->client_id();
    spdlog::info("[RPCServer] Disconnect request for client_id={}", client_id);

    // 调用 session_manager 移除 session
    session_manager::get_instance().remove_session(client_id); // 返回结果
    response->set_success(true);
    return grpc::Status::OK;
}

grpc::Status RPCServer::KeepAlive(grpc::ServerContext* context,
    const KeepAliveRequest* request, KeepAliveResponse* response)
{
    const std::string& client_id = request->client_id();
    spdlog::debug("[RPCServer] KeepAlive request for client_id={}", client_id);

    // 更新保活时间
    session_manager::get_instance().update_keepalive(client_id); // 返回结果
    response->set_success(true);
    return grpc::Status::OK;
}