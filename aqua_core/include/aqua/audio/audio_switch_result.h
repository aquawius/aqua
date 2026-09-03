#ifndef AQUA_AUDIO_SWITCH_RESULT_H
#define AQUA_AUDIO_SWITCH_RESULT_H

// 切换结果共享类型：playback（PlaybackManager）与 capture（CaptureManager）
// 两侧的 restart 事务使用同一份结果词汇（playback_switching_design.md §9 /
// capture_switching_design.md §5）。两侧设计文档独立成文，但结果类型共享，
// 避免同名枚举在两个头文件中重复定义造成 ODR 冲突。

#include "aqua/audio/audio_error.h"

#include <cstdint>

namespace aqua::audio {

// 切换结果：成功路径的降级信息。
enum class SwitchOutcome : std::uint8_t {
    None, // 尚未发生切换事务（start 后的初始状态）
    Switched, // 目标设备（或同设备 restart）一次成功
    RolledBack, // 目标失败，回滚 previous_active_device 成功
    FellBackToSystem, // 目标与回滚均失败，落系统默认成功
    Fatal, // 候选链耗尽（格式不兼容 / 重试超限）
};

struct SwitchResult {
    SwitchOutcome outcome = SwitchOutcome::None;
    AudioError last_error = AudioError::None; // 链上最后一次失败原因
};

[[nodiscard]] constexpr const char* switch_outcome_name(SwitchOutcome outcome) noexcept
{
    switch (outcome) {
    case SwitchOutcome::None:
        return "none";
    case SwitchOutcome::Switched:
        return "switched";
    case SwitchOutcome::RolledBack:
        return "rolled_back";
    case SwitchOutcome::FellBackToSystem:
        return "fell_back_to_system";
    case SwitchOutcome::Fatal:
        return "fatal";
    }
    return "unknown";
}

} // namespace aqua::audio

#endif // AQUA_AUDIO_SWITCH_RESULT_H
