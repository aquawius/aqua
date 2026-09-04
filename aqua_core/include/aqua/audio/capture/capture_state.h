#ifndef AQUA_AUDIO_CAPTURE_CAPTURE_STATE_H
#define AQUA_AUDIO_CAPTURE_CAPTURE_STATE_H

// CaptureSwitchState：采集生命周期的管理级状态维度
// （doc/capture_switching_design.md §7，对称 playback 侧 playback_state.h）。
//
// 两个正交状态维度，独立观测：
//   - 流级 AudioCaptureState（Active/Silent/Starved，audio_capture.h）：
//     backend 内部时间轴状态，描述采集时间轴是否在推进；
//   - 管理级 CaptureSwitchState（本文件）：描述 ServerRuntime 内 capture
//     流的管理状态，由 CaptureManager 维护。
//
// RuntimeState 语义收窄为"网络会话生命"；典型组合：
//   - RuntimeState=Running + CaptureSwitchState=Running    正常采集
//   - RuntimeState=Running + CaptureSwitchState=Switching  设备切换事务中
//     （旧流已停、新流未成；本窗口内到达的错误事件是旧流滞留错误，
//     由 ServerRuntime::on_capture_event 的 Switching gate 丢弃）
//   - RuntimeState=Running + CaptureSwitchState=Fatal      瞬态：候选链
//     耗尽/预算超限，control timer 观察到后将 stop 整个 runtime（≤500ms）
//
// Fatal 不是普通 capture 失败，而是 restart 事务候选链耗尽的终态
// （capture_switching_design.md §5；无 capture 的 server 会话无意义）。

#include <cstdint>

namespace aqua::audio {

enum class CaptureSwitchState : std::uint8_t {
    Inactive, // 未启动 / 已停止
    Starting, // 首次 start 进行中
    Running, // 采集运行中
    Switching, // restart 事务进行中（旧流已停、新流未成）
    Fatal, // 切换事务终态：候选链耗尽或重试预算超限；runtime 终止前的最后状态
};

inline constexpr const char* capture_switch_state_name(CaptureSwitchState state) noexcept
{
    switch (state) {
    case CaptureSwitchState::Inactive:
        return "inactive";
    case CaptureSwitchState::Starting:
        return "starting";
    case CaptureSwitchState::Running:
        return "running";
    case CaptureSwitchState::Switching:
        return "switching";
    case CaptureSwitchState::Fatal:
        return "fatal";
    default:
        return "unknown";
    }
}

} // namespace aqua::audio

#endif // AQUA_AUDIO_CAPTURE_CAPTURE_STATE_H
