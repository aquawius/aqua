#ifndef AQUA_AUDIO_ERROR_H
#define AQUA_AUDIO_ERROR_H

// 跨平台音频错误码。
// 平台层细节（HRESULT / ALSA errno / AAudio result 等）由后端记录到日志，
// 本错误码只表达上层能够处理的类别。跨层传递用 std::expected<T, AudioError>。

namespace aqua::audio {

enum class AudioError {
    None = 0,

    DeviceNotFound, // 启动时指定设备不存在
    DeviceUnavailable, // 设备存在但当前无法打开（被占用/暂不可用）
    DeviceDisconnected, // 运行过程中设备消失/失效
    FormatUnsupported, // 请求的 AudioFormat 不被后端支持
    NotSupported, // 该平台/后端不支持此模式（如 Android 的 Loopback）
    PermissionDenied, // 麦克风/音频权限被拒（Android/macOS 需显式授权）
    AlreadyRunning, // 已在运行时再次 start()
    NotRunning, // 未运行时执行了依赖运行状态的操作
    InvalidArgument, // 配置非法（如 format 无效或回调参数不合法）
    BackendFailed, // 平台层失败（具体原因见日志）
};

inline constexpr const char* audio_error_name(AudioError error) noexcept
{
    switch (error) {
    case AudioError::None: return "none";
    case AudioError::DeviceNotFound: return "device_not_found";
    case AudioError::DeviceUnavailable: return "device_unavailable";
    case AudioError::DeviceDisconnected: return "device_disconnected";
    case AudioError::FormatUnsupported: return "format_unsupported";
    case AudioError::NotSupported: return "not_supported";
    case AudioError::PermissionDenied: return "permission_denied";
    case AudioError::AlreadyRunning: return "already_running";
    case AudioError::NotRunning: return "not_running";
    case AudioError::InvalidArgument: return "invalid_argument";
    case AudioError::BackendFailed: return "backend_failed";
    default: return "unknown";
    }
}

} // namespace aqua::audio

#endif // AQUA_AUDIO_ERROR_H
