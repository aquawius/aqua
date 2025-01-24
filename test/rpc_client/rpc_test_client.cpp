//
// Created by aquawius on 25-1-23.
//

#include "rpc_test_client.h"

#include <iostream>

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
        std::cerr << "[rpc_client] Connect RPC :"
                  << status.error_code() << " - " << status.error_message() << std::endl;
        return false;
    }
    if (!response.success()) {
        std::cerr << "[rpc_client] Connect Connect refused:"
                  << response.error_message() << std::endl;
        return false;
    }

    // 读取服务器分配的 UUID
    clientUuidOut = response.client_uuid();
    std::cout << "[rpc_client] Connect Success, client_uuid = " << clientUuidOut
              << ", Server Address=" << response.server_address()
              << ", Port=" << response.server_port() << std::endl;

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
        std::cerr << "[rpc_client] Disconnect RPC Fail:"
                  << status.error_code() << " - " << status.error_message() << std::endl;
        return false;
    }
    std::cout << "[rpc_client] Disconnect Seccess: success = "
              << (response.success() ? "true" : "false") << std::endl;
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
        std::cerr << "[rpc_client] KeepAlive RPC Fail:"
                  << status.error_code() << " - " << status.error_message() << std::endl;
        return false;
    }

    std::cout << "[rpc_client] KeepAlive Success: success = "
              << (response.success() ? "true" : "false") << std::endl;
    return response.success();
}
