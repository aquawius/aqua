#ifndef AQUA_AUDIO_PLAYBACK_PLAYBACK_ROUTE_MODE_H
#define AQUA_AUDIO_PLAYBACK_PLAYBACK_ROUTE_MODE_H

// PlaybackRouteMode：路由模式（playback_switching_design.md §4）。
//
// 路由是 connection 属性，不持久化（设备 id 跨会话不稳定）；每次连接按
// 设置起步。唯一持久化的是上层 UI 的"自动切换播放设备"开关。
//
// 错误驱动 restart 的目标由当前模式推导：
//   FollowSystem      -> 系统默认（device = nullopt）
//   PreferCurrent     -> 之前的实际设备 id
//   PreferredDevice   -> 指定 id（优先而非固定：不可用时按 fallback 链降级，
//                        不无限等待——"永不主动静音优先"）

#include <cstdint>
#include <string_view>

namespace aqua::audio {

enum class PlaybackRouteMode : std::uint8_t {
    FollowSystem, // 跟随系统默认（"自动切换"开）
    PreferCurrent, // 优先保持当前实际设备（"自动切换"关；首流成功后钉住实际设备，丢失则回退系统）
    PreferredDevice, // 优先指定设备（用户手动选择）
};

inline constexpr std::string_view playback_route_mode_name(PlaybackRouteMode mode) noexcept
{
    switch (mode) {
    case PlaybackRouteMode::FollowSystem:
        return "follow_system";
    case PlaybackRouteMode::PreferCurrent:
        return "prefer_current";
    case PlaybackRouteMode::PreferredDevice:
        return "preferred_device";
    default:
        return "unknown";
    }
}

} // namespace aqua::audio

#endif // AQUA_AUDIO_PLAYBACK_PLAYBACK_ROUTE_MODE_H
