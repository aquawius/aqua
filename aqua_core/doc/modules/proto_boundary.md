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

非法输入不再归一为哨兵值：`from_proto()` 返回 `std::expected<AudioFormat, AudioError>`（`[[nodiscard]]`）， 失败原因进类型——未知/非法
encoding → `FormatUnsupported`，channels / sample_rate 越界 → `InvalidArgument`。 下游因此拿不到"encoding
合法但维度非法"的半成品，也不需要再靠 `is_valid()` 兜底。`to_proto()` 方向不变： 未知 encoding 映射为 `ENCODING_INVALID`。
