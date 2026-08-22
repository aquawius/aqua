#ifndef AQUA_NET_GRPC_INCLUDE_H
#define AQUA_NET_GRPC_INCLUDE_H

// gRPC 头文件的统一包含点。所有需要 gRPC 的代码（src 与 tests）都应包含本头，
// 不要直接包含 <grpcpp/grpcpp.h> / <aqua_service.grpc.pb.h>。
//
// 原因：gRPC 上游头（grpcpp/security/tls_certificate_provider.h 等）在 MSVC /W4 下
// 会触发 C4996（[[deprecated]] 的 IdentityKeyCertPair 等）噪音警告，且这些警告不
// 属于本项目代码。这里只在包含第三方头的那一小段范围内用 pragma 抑制，项目其余
// 代码的警告级别不受影响；gRPC 升级消除弃用后，删除 push/disable/pop 即可恢复。
// 该头不抑制任何警告等级，只临时关闭 4996。

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996) // [[deprecated]] 弃用警告（gRPC 上游头触发）
#endif

#include <aqua_service.grpc.pb.h>
#include <grpcpp/grpcpp.h>

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#endif // AQUA_NET_GRPC_INCLUDE_H
