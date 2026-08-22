#ifndef AQUA_AUDIO_CAPTURE_H
#define AQUA_AUDIO_CAPTURE_H

// 输入流抽象（OS --push--> 应用）。
//
// 线程模型与回调生命周期（实现必须遵守）：
//   - frame_callback 运行在后端的实时音频线程上，禁止阻塞（不得加锁/堆分配/IO）；
//   - frame.data 仅在回调内有效，需要保留的数据必须自行拷贝；
//   - 回调签名 noexcept：任何异常都不得越过回调（实现负责捕获并记录）；
//   - start() 成功前不会触发回调；stop() 返回时保证回调不再被调用
//     （实现需等待音频线程退出后再返回）；
//   - stop() 后可再次 start()（同一实例复用）。
//
// 运行期错误（设备被拔出 / 音频服务重启等）通过 event_callback 投递，
// 与 start() 的同步返回值互补（start() 只覆盖启动期失败）：
//   - event_callback 运行在 backend 的内部线程（已退出实时音频数据路径），
//     不是调用 start()/stop() 的控制线程，回调必须快速返回；
//   - 不得在 frame_callback / event_callback 内调用 stop() 或 start()
//     （stop() 会 join 该线程导致自死锁）；
//   - event_callback 为 nullptr 时，运行期错误仅记日志，上层可通过
//     is_running() 轮询感知流已停止。

#include "aqua/audio/audio_error.h"
#include "aqua/audio/audio_frame.h"
#include "aqua/audio/capture/audio_capture_config.h"

#include <expected>
#include <memory>

namespace aqua::audio {

class AudioDeviceManager;

// 采集帧回调：后端在实时音频线程上推入一帧数据。
// 用 C 函数指针而非 std::function，避免实时路径上的分配与间接层。
using AudioCaptureCallback = void (*)(void* user_data, const AudioFrame& frame) noexcept;

// 运行期事件回调：异步错误（DeviceDisconnected / BackendFailed 等）。
using AudioCaptureEventCallback = void (*)(void* user_data, AudioError error) noexcept;

// 输入流抽象。跨平台接口，具体实现见 src/audio/capture/<backend>/。
class AudioCapture {
public:
    virtual ~AudioCapture() = default;

    // 以 config 启动采集。
    // frame_callback == nullptr 或 config 非法 -> InvalidArgument。
    // 设备不存在/失效 -> DeviceNotFound；占用 -> DeviceUnavailable；
    // 格式不支持 -> FormatUnsupported；方向不支持（如 Android 的 loopback）-> NotSupported；
    // 权限被拒 -> PermissionDenied；已在运行 -> AlreadyRunning。
    // 成功返回后帧回调即开始被调用，且交付的 PCM 严格等于 config.format（capture 不做转换）。
    virtual std::expected<void, AudioError>
    start(const AudioCaptureConfig& config,
        AudioCaptureCallback frame_callback,
        void* frame_user_data,
        AudioCaptureEventCallback event_callback = nullptr,
        void* event_user_data = nullptr) noexcept = 0;

    [[nodiscard]] virtual bool is_running() const noexcept = 0;

    // 停止采集并等待回调线程退出。未运行时调用为 no-op。可再次 start()。
    // 不得从 frame_callback / event_callback 内直接调用 stop()。
    virtual void stop() noexcept = 0;
};

// 工厂：创建当前平台的采集后端（如 WASAPI）。device_manager 必须在本对象生命周期内保持有效。
// 该平台尚未实现时返回 nullptr。
std::unique_ptr<AudioCapture> create_capture(AudioDeviceManager& device_manager);

} // namespace aqua::audio

#endif // AQUA_AUDIO_CAPTURE_H
