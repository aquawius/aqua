//
// Created by aquawius on 25-1-20.
//

#ifndef AUDIO_SERVICE_IMPL_H
#define AUDIO_SERVICE_IMPL_H

#include <grpcpp/grpcpp.h>
#include "proto_gen/audio_service.grpc.pb.h"

class network_server;

using AudioService::auqa::pb::ConnectRequest;
using AudioService::auqa::pb::ConnectResponse;
using AudioService::auqa::pb::DisconnectRequest;
using AudioService::auqa::pb::DisconnectResponse;
using AudioService::auqa::pb::KeepAliveRequest;
using AudioService::auqa::pb::KeepAliveResponse;

// 基于 proto 生成的服务类 AudioService 的实现
class RPCServer final : public AudioService::auqa::pb::AudioService::Service {
public:
    explicit RPCServer(network_server& manager);

    grpc::Status Connect(grpc::ServerContext* context,
        const ConnectRequest* request,
        ConnectResponse* response) override;

    grpc::Status Disconnect(grpc::ServerContext* context,
        const DisconnectRequest* request,
        DisconnectResponse* response) override;

    grpc::Status KeepAlive(grpc::ServerContext* context,
        const KeepAliveRequest* request,
        KeepAliveResponse* response) override;

private:
    network_server& m_network_manager;
};
#endif // AUDIO_SERVICE_IMPL_H
