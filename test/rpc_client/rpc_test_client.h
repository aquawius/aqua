//
// Created by aquawius on 25-1-23.
//

#ifndef RPC_TEST_CLIENT_H
#define RPC_TEST_CLIENT_H

#include <grpcpp/grpcpp.h>
#include <string>
#include "proto_gen/audio_service.grpc.pb.h"

class TestRpcClient {
public:
    // 通过传入已创建的 channel 来实例化客户端
    explicit TestRpcClient(std::shared_ptr<grpc::Channel> channel);

    // 向服务器发送 Connect
    // 返回 true 表示成功，clientUuidOut 将保存从服务器获取到的 UUID
    bool Connect(const std::string& clientAddress,
        uint32_t clientPort,
        std::string& clientUuidOut);

    // 向服务器发送 Disconnect
    bool Disconnect(const std::string& clientUuid);

    // 向服务器发送 KeepAlive
    bool KeepAlive(const std::string& clientUuid);

private:
    std::unique_ptr<AudioService::auqa::pb::AudioService::Stub> m_stub;
};

#endif // RPC_TEST_CLIENT_H
