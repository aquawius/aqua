//
// Created by aquawius on 25-1-29.
//

#include "rpc_client.h"

#include <audio_playback.h>
#include <grpcpp/grpcpp.h>
#include <spdlog/spdlog.h>

rpc_client::rpc_client(std::shared_ptr<grpc::Channel> channel)
    : m_stub(AudioService::auqa::pb::AudioService::NewStub(channel)) {}

static AudioService::auqa::pb::AudioFormat_Encoding convert_encoding_to_proto(audio_playback::AudioEncoding encoding)
{
    using ProtoEncoding = AudioService::auqa::pb::AudioFormat_Encoding;

    switch (encoding) {
    case audio_playback::AudioEncoding::PCM_S16LE:
        return ProtoEncoding::AudioFormat_Encoding_ENCODING_PCM_S16LE;
    case audio_playback::AudioEncoding::PCM_S32LE:
        return ProtoEncoding::AudioFormat_Encoding_ENCODING_PCM_S32LE;
    case audio_playback::AudioEncoding::PCM_F32LE:
        return ProtoEncoding::AudioFormat_Encoding_ENCODING_PCM_F32LE;
    case audio_playback::AudioEncoding::PCM_S24LE:
        return ProtoEncoding::AudioFormat_Encoding_ENCODING_PCM_S24LE;
    case audio_playback::AudioEncoding::PCM_U8:
        return ProtoEncoding::AudioFormat_Encoding_ENCODING_PCM_U8;
    default:
        return ProtoEncoding::AudioFormat_Encoding_ENCODING_INVALID;
    }
}

// 辅助函数：将protobuf中的编码类型转换为audio_manager中的AudioEncoding
static audio_playback::AudioEncoding convert_proto_to_encoding(AudioService::auqa::pb::AudioFormat_Encoding encoding)
{
    using Encoding = audio_playback::AudioEncoding;

    switch (encoding) {
    case AudioService::auqa::pb::AudioFormat_Encoding_ENCODING_PCM_S16LE:
        return Encoding::PCM_S16LE;
    case AudioService::auqa::pb::AudioFormat_Encoding_ENCODING_PCM_S32LE:
        return Encoding::PCM_S32LE;
    case AudioService::auqa::pb::AudioFormat_Encoding_ENCODING_PCM_F32LE:
        return Encoding::PCM_F32LE;
    case AudioService::auqa::pb::AudioFormat_Encoding_ENCODING_PCM_S24LE:
        return Encoding::PCM_S24LE;
    case AudioService::auqa::pb::AudioFormat_Encoding_ENCODING_PCM_U8:
        return Encoding::PCM_U8;
    default:
        return Encoding::INVALID;
    }
}


bool rpc_client::connect(const std::string& clientAddress,
    uint32_t clientPort,
    std::string& clientUuidOut,
    AudioService::auqa::pb::AudioFormat& serverFormat)
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

    // 获取服务器音频格式
    if (response.has_server_format()) {
        serverFormat = response.server_format();
        spdlog::info("[rpc_client] Server audio format recived.");

        audio_common::AudioFormat format(serverFormat);
        spdlog::info("[rpc_client] Server audio format recived: {} Hz, {} ch, {} bit, {}",
            format.sample_rate,
            format.channels,
            format.bit_depth,
            audio_common::AudioFormat::is_float_encoding(format.encoding).value_or(false) ? "float" : "int");

    } else {
        spdlog::warn("[rpc_client] No audio format received from server");
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
        spdlog::error("[rpc_client] KeepAlive RPC Fail: {} - {}",
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

    return true;
}

// 添加获取音频格式的方法
bool rpc_client::get_audio_format(const std::string& clientUuid,
    AudioService::auqa::pb::AudioFormat& formatOut)
{
    AudioService::auqa::pb::GetAudioFormatRequest request;
    request.set_client_uuid(clientUuid);

    AudioService::auqa::pb::AudioFormatResponse response;
    grpc::ClientContext context;

    grpc::Status status = m_stub->GetAudioFormat(&context, request, &response);
    if (!status.ok()) {
        spdlog::error("[rpc_client] GetAudioFormat RPC Fail: {} - {}",
            static_cast<int>(status.error_code()),
            status.error_message());
        return false;
    }

    if (!response.has_format()) {
        spdlog::error("[rpc_client] GetAudioFormat has NO format return: {}",
            response.error_message());
        return false;
    }

    formatOut = response.format();

    audio_common::AudioFormat format(formatOut);
    spdlog::info("[rpc_client] Server audio format recived: {} Hz, {} ch, {} bit, {}",
        format.sample_rate,
        format.channels,
        format.bit_depth,
        audio_common::AudioFormat::is_float_encoding(format.encoding).value_or(false) ? "float" : "int");
    return true;
}