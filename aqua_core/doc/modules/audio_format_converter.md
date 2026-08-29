# 模块：AudioFormat Converter

## 文件

`aqua_core/src/audio/audio_format_converter.cpp`

## 职责

把 protobuf 的 `pb::AudioFormat` 与 Core 的 `audio::AudioFormat` 互转。它是 gRPC 边界层，不属于 audio domain 本身。

## from_proto

proto3 的 `uint32 channels/sample_rate` 本身没有 Aqua 业务范围限制，因此必须在转换层再次检查：

```text
1 <= channels <= 64
1 <= sample_rate <= 768000
```

encoding 只接受已知 PCM 枚举。

任何非法组合都统一返回：

```text
encoding = INVALID
channels = 0
sample_rate = 0
```

这样下游可以只用 `AudioFormat::is_valid()` 判断，而不会拿到“encoding 合法但 dimensions 非法”的半成品。

## to_proto

Core enum 与 proto enum 是一一对应映射。任何未知/非法 encoding 映射为 `ENCODING_INVALID`。

## 架构意义

`aqua_core_base` 不依赖 protobuf；只有 `aqua_proto` 同时包含 generated proto/gRPC 和 converter。这让普通 audio/network/runtime 模块不被 protobuf ABI 污染。
