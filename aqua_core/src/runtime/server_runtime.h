#ifndef AQUA_SERVER_RUNTIME_H
#define AQUA_SERVER_RUNTIME_H

namespace aqua::runtime {

// Server-side orchestration will own the capture/session/network pipeline.
// Low-level WASAPI realtime thread code intentionally remains in the audio backend.
class ServerRuntime final {
public:
    ServerRuntime() = default;
    ~ServerRuntime() = default;

    ServerRuntime(const ServerRuntime&) = delete;
    ServerRuntime& operator=(const ServerRuntime&) = delete;
};

} // namespace aqua::runtime

#endif // AQUA_SERVER_RUNTIME_H
