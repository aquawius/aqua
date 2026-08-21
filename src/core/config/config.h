#ifndef AQUA_CONFIG_H
#define AQUA_CONFIG_H

namespace aqua::config {

// gRPC建立连接的连接超时
inline constexpr std::chrono::milliseconds GRPC_CONNECT_DEADLINE { 3000 };

inline constexpr std::chrono::milliseconds GRPC_DISCONNECT_DEADLINE { 1000 };

}


#endif // AQUA_CONFIG_H
