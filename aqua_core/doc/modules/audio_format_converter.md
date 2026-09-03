# 模块：AudioFormat Converter

## 文件

- `aqua_core/src/audio/audio_format_converter.cpp`
- `aqua_core/proto/aqua_service.proto`（`AudioFormat` 消息）

## 职责

在 protobuf 的 `pb::AudioFormat` 与 Core 的 `audio::AudioFormat` 之间互转。它是 gRPC 边界层，不属于 audio domain 本身。

## from_proto

proto3 的 `uint32 channels` / `sample_rate` 本身没有 Aqua 业务范围限制，必须在转换层重新校验：

```text
1 <= channels    <= 64       (AUDIO_FORMAT_MAX_CHANNELS)
1 <= sample_rate <= 768000   (AUDIO_FORMAT_MAX_SAMPLE_RATE)
```

encoding 只接受已知的 PCM 枚举；未知值按非法处理。任何非法组合统一返回：

```text
encoding    = INVALID
channels    = 0
sample_rate = 0
```

这样下游只需用 `AudioFormat::is_valid()` 判定，不会拿到"encoding 合法但维度非法"的半成品。

## to_proto

Core 枚举与 proto 枚举一一对应：

```text
INVALID / PCM_S16LE / PCM_S32LE / PCM_F32LE / PCM_S24LE / PCM_U8
    ↔  ENCODING_INVALID / ENCODING_PCM_S16LE / ... / ENCODING_PCM_U8
```

任何未知或非法 encoding 映射为 `ENCODING_INVALID`。注意两侧枚举值必须同步修改——`audio_format.h` 与 proto 文件都有此约束
注释。

## 架构意义

`aqua_core_base` 不依赖 protobuf：converter 与生成的 proto/gRPC 代码同属 `aqua_proto` 目标，audio / net / runtime 模块因此
不被 protobuf ABI 污染。
