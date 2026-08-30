# 模块：Protobuf Boundary

`aqua_core/proto/aqua_service.proto` 是控制面 wire schema。原生 Core 使用 `AudioFormat`，通过 `audio_format_converter.cpp`
做双向转换。

## 为什么转换层独立

`aqua_core_base` 不依赖 protobuf 生成物；只有 gRPC client/server 需要 proto。因此 `aqua_proto` 单独承载：

- proto source
- generated protobuf/gRPC
- AudioFormat converter

这样公共 base 可以保持对协议编译栈的隔离。

## 输入校验

proto3 的 uint32 没有业务范围约束，因此 `from_proto()` 必须重新验证：

- encoding 是否是已知枚举；
- channels 在 1..64；
- sample_rate 在 1..768000。

非法输入统一归一为 INVALID/0/0，由调用方 `is_valid()` 拒绝。
