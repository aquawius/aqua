//
// Created by aquawius on 25-1-29.
//

#include "rpc_client.h"

#include <grpcpp/grpcpp.h>
#include <spdlog/spdlog.h>

rpc_client::rpc_client(std::shared_ptr<grpc::Channel> channel)
    : m_stub(AudioService::auqa::pb::AudioService::NewStub(channel))
{
}

bool rpc_client::connect(const std::string& clientAddress,
    uint32_t clientPort,
    std::string& clientUuidOut)
{
    AudioService::auqa::pb::ConnectRequest request;
    request.set_client_address(clientAddress);
    request.set_client_port(clientPort);

    AudioService::auqa::pb::ConnectResponse response;
    grpc::ClientContext context;

    grpc::Status status = m_stub->Connect(&context, request, &response);
    if (!status.ok()) {
        spdlog::error("[rpc_client] Connect RPC : {} - {}",
            static_cast<int>(status.error_code()), status.error_message());
        return false;
    }

    if (!response.success()) {
        spdlog::error("[rpc_client] Connect refused: {}", response.error_message());
        return false;
    }

    clientUuidOut = response.client_uuid();
    spdlog::info("[rpc_client] Connect Success, client_uuid = {}, "
                 "Server Address={}, Port={}",
        clientUuidOut,
        response.server_address(),
        response.server_port());
    return true;
}

bool rpc_client::disconnect(const std::string& clientUuid)
{
    AudioService::auqa::pb::DisconnectRequest request;
    request.set_client_uuid(clientUuid);

    AudioService::auqa::pb::DisconnectResponse response;
    grpc::ClientContext context;

    grpc::Status status = m_stub->Disconnect(&context, request, &response);
    if (!status.ok()) {
        spdlog::error("[rpc_client] Disconnect RPC Fail: {} - {}",
            static_cast<int>(status.error_code()), status.error_message());
        return false;
    }

    spdlog::info("[rpc_client] Disconnect Success: success = {}",
        response.success() ? "true" : "false");
    return response.success();
}

bool rpc_client::keep_alive(const std::string& clientUuid)
{
    AudioService::auqa::pb::KeepAliveRequest request;
    request.set_client_uuid(clientUuid);

    AudioService::auqa::pb::KeepAliveResponse response;
    grpc::ClientContext context;

    grpc::Status status = m_stub->KeepAlive(&context, request, &response);
    if (!status.ok()) {
        spdlog::error("[rpc_client] KeepAlive RPC Fail: Code={}, Message={}",
            static_cast<int>(status.error_code()),
            status.error_message());
        return false;
    }

    if (!response.success()) {
        // 提取服务器返回的具体错误信息
        std::string error_msg = response.error_message();
        spdlog::error("[rpc_client] KeepAlive refused: {}", error_msg.empty() ? error_msg : "Unknown Error");
        return false;
    }

    spdlog::info("[rpc_client] KeepAlive Success: success=true");
    return true;
}