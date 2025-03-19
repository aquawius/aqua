//
// Created by aquawius on 25-1-29.
//

#ifndef RPC_CLIENT_H
#define RPC_CLIENT_H

#include "audio_service.grpc.pb.h"

#include <memory>
#include <string>

#include <grpcpp/grpcpp.h>

class rpc_client {
public:
    // 通过传入已创建的 channel 来实例化客户端
    explicit rpc_client(std::shared_ptr<grpc::Channel> channel);

    // 向服务器发送 Connect
    // 返回 true 表示成功，clientUuidOut 将保存从服务器获取到的 UUID
    // 现在同时返回服务器的音频格式
    bool connect(const std::string& clientAddress,
        uint32_t clientPort,
        std::string& clientUuidOut,
        AudioService::auqa::pb::AudioFormat& serverFormat);

    // 向服务器发送 Disconnect
    bool disconnect(const std::string& clientUuid);

    // 向服务器发送 KeepAlive
    bool keep_alive(const std::string& clientUuid);
    
    // 向服务器获取音频格式
    bool get_audio_format(const std::string& clientUuid, 
        AudioService::auqa::pb::AudioFormat& formatOut);

private:
    std::unique_ptr<AudioService::auqa::pb::AudioService::Stub> m_stub;
};

#endif // RPC_CLIENT_H
