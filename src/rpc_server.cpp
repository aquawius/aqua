//
// Created by aquawius on 25-1-20.
//

#include "rpc_server.h"
#include "network_server.h"
#include "session_manager.h"

#include <boost/asio.hpp>
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/random_generator.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <spdlog/spdlog.h>

RPCServer::RPCServer(network_server& manager)
    : m_network_manager(manager)
{
}

grpc::Status RPCServer::Connect(grpc::ServerContext* context,
    const ConnectRequest* request,
    ConnectResponse* response)
{
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

    boost::asio::ip::udp::endpoint endpoint(address, client_port);
    boost::uuids::uuid uuid = boost::uuids::random_generator()();
    std::string client_uuid_str = boost::uuids::to_string(uuid);
    spdlog::info("[rpc_server] Generated client UUID: {}", client_uuid_str);

    if (!session_manager::get_instance().add_session(client_uuid_str, endpoint)) {
        return grpc::Status { grpc::StatusCode::ALREADY_EXISTS, "Cannot add session" };
    }

    response->set_success(true);
    response->set_error_message("OK");
    response->set_client_uuid(client_uuid_str);
    // TODO: custom ip address
    response->set_server_address(network_server::get_default_address());
    // TODO: custom port
    response->set_server_port(10120);

    return grpc::Status::OK;
}

grpc::Status RPCServer::Disconnect(grpc::ServerContext* context,
    const DisconnectRequest* request,
    DisconnectResponse* response)
{
    const std::string& client_uuid = request->client_uuid();
    spdlog::info("[rpc_server] Disconnect request for client_uuid={}", client_uuid);

    session_manager::get_instance().remove_session(client_uuid);
    response->set_success(true);
    return grpc::Status::OK;
}

grpc::Status RPCServer::KeepAlive(grpc::ServerContext* context,
    const KeepAliveRequest* request,
    KeepAliveResponse* response)
{
    const std::string& client_uuid = request->client_uuid();
    spdlog::debug("[rpc_server] KeepAlive request for client_uuid={}", client_uuid);

    session_manager::get_instance().update_keepalive(client_uuid);
    response->set_success(true);
    return grpc::Status::OK;
}
