#ifndef AQUA_AUDIO_CAPTURE_AUDIO_CAPTURE_CONFIG_H
#define AQUA_AUDIO_CAPTURE_AUDIO_CAPTURE_CONFIG_H

#include "aqua/audio/audio_format.h"
#include "aqua/audio/devices/audio_route.h"

#include <cstdint>
#include <optional>

namespace aqua::audio {

enum class AudioCaptureSource {
    INPUT_DEVICE,
    OUTPUT_LOOPBACK,
};

struct AudioCaptureConfig {
    // 输入设备采集，或输出设备的系统混音（loopback）。
    // Loopback 不是一个独立 AudioDevice，而是一种 capture source。
    // Generic AudioCapture API default is input; ServerRuntime overrides this to OUTPUT_LOOPBACK
    // for Aqua product startup defaults.
    AudioCaptureSource source = AudioCaptureSource::INPUT_DEVICE;

    // 路由请求（唯一的设备选择入口）。server 侧的 RoutePolicy 把
    // Application 解释为严格钉住：选定的采集源丢失即 Fatal，绝不静默换源
    // （采集内容与用户预期不符且无从告知）。
    AudioRoute route = AudioRoute::follow_system();

    // 请求的采集格式。std::nullopt 表示由 backend 使用该 stream 的默认/shared-mode 格式。
    // capture 不做格式转换；指定 format 时必须由 backend 原生支持，否则返回 FormatUnsupported。
    std::optional<AudioFormat> format;

    // 请求的缓冲大小（帧）；实际回调粒度由后端决定。
    //
    // WASAPI 采集后端是唯一的消费者，但 CLI/C API 目前不暴露采集侧旋钮 ⇒ 本值恒为 0。
    // 它的含义**不是**"0 就是最小缓冲"：旧式 `Initialize` 那条路被 WASAPI 强制要求把 buffer
    // duration 与 period 都设为 0（文档原文：caller must set both to 0），此时缓冲大小由
    // **引擎的调度周期**决定 = 引擎**默认**周期，不是最小周期。
    //   - INPUT_DEVICE：另走 IAudioClient3，0 ⇒ 取**设备最小周期**（实机实测 period
    //     480→336 帧、端点缓冲 1056→738 帧）；非 0 按 `choose_engine_period()` 折算到最近的
    //     fundamental 倍数并钳到 [min, max]；
    //   - OUTPUT_LOOPBACK（server 默认）：只能停在引擎默认周期 —— `InitializeSharedAudioStream`
    //     只接受 AUDCLNT_STREAMFLAGS_EVENTCALLBACK，带 LOOPBACK 返回
    //     AUDCLNT_E_INVALID_STREAM_FLAG（实机实测），而 loopback 必须要它。
    // 实测数值与 API 行为见 `doc/modules/capture.md` 的「设备缓冲（引擎周期）停在哪里」。
    std::uint32_t frames_per_buffer = 0;
};

} // namespace aqua::audio

#endif //  AQUA_AUDIO_CAPTURE_AUDIO_CAPTURE_CONFIG_H
