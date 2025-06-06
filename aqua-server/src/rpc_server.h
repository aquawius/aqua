//
// Created by aquawius on 25-1-20.
//

#ifndef AUDIO_SERVICE_IMPL_H
#define AUDIO_SERVICE_IMPL_H

#include "audio_format_common.hpp"
#include "audio_manager.h"
#include "audio_service.grpc.pb.h"

class network_server;
class audio_manager;

using AudioService::auqa::pb::ConnectRequest;
using AudioService::auqa::pb::ConnectResponse;
using AudioService::auqa::pb::DisconnectRequest;
using AudioService::auqa::pb::DisconnectResponse;
using AudioService::auqa::pb::KeepAliveRequest;
using AudioService::auqa::pb::KeepAliveResponse;
using AudioService::auqa::pb::GetAudioFormatRequest;
using AudioService::auqa::pb::AudioFormatResponse;

// 基于 proto 生成的服务类 AudioService 的实现
class RPCServer final : public AudioService::auqa::pb::AudioService::Service
{
public:
    // 使用共享命名空间中的类型
    using AudioEncoding = audio_common::AudioEncoding;

    explicit RPCServer(network_server& manager, std::shared_ptr<audio_manager> audio_mgr);
    ~RPCServer() override;

    grpc::Status Connect(grpc::ServerContext* context,
                         const ConnectRequest* request,
                         ConnectResponse* response) override;

    grpc::Status Disconnect(grpc::ServerContext* context,
                            const DisconnectRequest* request,
                            DisconnectResponse* response) override;

    grpc::Status KeepAlive(grpc::ServerContext* context,
                           const KeepAliveRequest* request,
                           KeepAliveResponse* response) override;

    grpc::Status GetAudioFormat(grpc::ServerContext* context,
                                const GetAudioFormatRequest* request,
                                AudioFormatResponse* response) override;

    // 工具函数 - 使用公共命名空间中的函数
    static AudioService::auqa::pb::AudioFormat_Encoding convert_encoding_to_proto(
        AudioEncoding encoding)
    {
        return audio_common::convert_encoding_to_proto(encoding);
    }

    static AudioEncoding convert_proto_to_encoding(
        AudioService::auqa::pb::AudioFormat_Encoding encoding)
    {
        return audio_common::convert_proto_to_encoding(encoding);
    }

private:
    network_server& m_network_manager;
    std::shared_ptr<audio_manager> m_audio_manager;
};
#endif // AUDIO_SERVICE_IMPL_H
