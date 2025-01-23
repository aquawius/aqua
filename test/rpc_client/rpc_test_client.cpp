//
// Created by aquawius on 25-1-23.
//

#include "rpc_test_client.h"

#include <iostream>

TestRpcClient::TestRpcClient(std::shared_ptr<grpc::Channel> channel)
    : m_stub(AudioService::auqa::pb::AudioService::NewStub(channel))
{
}

bool TestRpcClient::Connect(const std::string& clientAddress,
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
        std::cerr << "[TestRpcClient] Connect RPC :"
                  << status.error_code() << " - " << status.error_message() << std::endl;
        return false;
    }
    if (!response.success()) {
        std::cerr << "[TestRpcClient] Connect Connect refused:"
                  << response.error_message() << std::endl;
        return false;
    }

    // 读取服务器分配的 UUID
    clientUuidOut = response.client_uuid();
    std::cout << "[TestRpcClient] Connect Success, client_uuid = " << clientUuidOut
              << ", Server Address=" << response.server_address()
              << ", Port=" << response.server_port() << std::endl;

    return true;
}

bool TestRpcClient::Disconnect(const std::string& clientUuid)
{
    AudioService::auqa::pb::DisconnectRequest request;
    request.set_client_uuid(clientUuid);

    AudioService::auqa::pb::DisconnectResponse response;
    grpc::ClientContext context;

    grpc::Status status = m_stub->Disconnect(&context, request, &response);
    if (!status.ok()) {
        std::cerr << "[TestRpcClient] Disconnect RPC Fail:"
                  << status.error_code() << " - " << status.error_message() << std::endl;
        return false;
    }
    std::cout << "[TestRpcClient] Disconnect Seccess: success = "
              << (response.success() ? "true" : "false") << std::endl;
    return response.success();
}

bool TestRpcClient::KeepAlive(const std::string& clientUuid)
{
    AudioService::auqa::pb::KeepAliveRequest request;
    request.set_client_uuid(clientUuid);

    AudioService::auqa::pb::KeepAliveResponse response;
    grpc::ClientContext context;

    grpc::Status status = m_stub->KeepAlive(&context, request, &response);
    if (!status.ok()) {
        std::cerr << "[TestRpcClient] KeepAlive RPC Fail:"
                  << status.error_code() << " - " << status.error_message() << std::endl;
        return false;
    }

    std::cout << "[TestRpcClient] KeepAlive Success: success = "
              << (response.success() ? "true" : "false") << std::endl;
    return response.success();
}