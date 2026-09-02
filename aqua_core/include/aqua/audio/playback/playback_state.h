#ifndef AQUA_AUDIO_PLAYBACK_PLAYBACK_STATE_H
#define AQUA_AUDIO_PLAYBACK_PLAYBACK_STATE_H

// PlaybackState：本地播放生命周期的平行状态维度（见 doc/playback_switching_design.md §3）。
//
// RuntimeState 语义收窄为“网络会话生命”；PlaybackState 描述 ClientRuntime 内
// playback 流的管理状态，两者独立观测：
//   - RuntimeState=Running + PlaybackState=Running   正常播放
//   - RuntimeState=Running + PlaybackState=Switching 设备切换中（网络正常）
//   - RuntimeState=Running + PlaybackState=Fatal     瞬态：fallback 链耗尽，
//     supervision 观察到后将 stop 整个 runtime（≤500ms）
//
// Fatal 不是普通 playback 失败，而是 restart 事务 fallback 链耗尽的终态
// （playback_switching_design.md §3.2）。

#include <cstdint>

namespace aqua::audio {

enum class PlaybackState : std::uint8_t {
    Inactive, // 未启动（连接前 / 已停止）
    Starting, // 首次启动中
    Running, // 流在跑
    Switching, // restart 事务进行中（旧流已停、新流未成）
    Fatal, // 切换事务终态：候选链耗尽（格式不兼容）或错误重启重试超限；runtime 终止前的最后状态
};

inline constexpr const char* playback_state_name(PlaybackState state) noexcept
{
    switch (state) {
    case PlaybackState::Inactive:
        return "inactive";
    case PlaybackState::Starting:
        return "starting";
    case PlaybackState::Running:
        return "running";
    case PlaybackState::Switching:
        return "switching";
    case PlaybackState::Fatal:
        return "fatal";
    default:
        return "unknown";
    }
}

} // namespace aqua::audio

#endif // AQUA_AUDIO_PLAYBACK_PLAYBACK_STATE_H
