#ifndef AQUA_AUDIO_CAPTURE_CAPTURE_ROUTE_MODE_H
#define AQUA_AUDIO_CAPTURE_CAPTURE_ROUTE_MODE_H

// CaptureRouteMode：采集路由模式（doc/capture_switching_design.md §4，
// 对称 playback 侧 playback_route_mode.h；两侧取舍差异是有意为之）。
//
// 路由来自 CLI 启动配置（--device-id），运行期无手动切换入口，sticky =
// 配置值（server 无交互界面；未来 GUI/Web server 可自行暴露入口，
// 不推翻 core 契约）。
//
// 错误驱动 restart 的目标由当前模式推导：
//   FollowSystem    -> 系统默认（device = nullopt）
//   PreferredDevice -> sticky 配置设备
//
// 与 playback 侧的两处不对称（capture_switching_design.md §14.5）：
//   - 无 PreferCurrent：server 无 UI，无"保持当前"的用户语义；
//   - PreferredDevice 是钉住而非优先：设备不可用即 Fatal（stop），绝不
//     降级到系统默认（CLI 指定设备 = "只要这个设备的数据"，静默换源
//     会让采集内容与用户预期不符且无从告知）；playback 的
//     PreferredDevice 是"优先 + fallback"（移动端"永不主动静音"优先）。

#include <cstdint>

namespace aqua::audio {

enum class CaptureRouteMode : std::uint8_t {
    FollowSystem, // 跟随 source 方向的系统默认设备
    PreferredDevice, // 钉住指定设备（sticky；不可用即 Fatal，不降级）
};

inline constexpr const char* capture_route_mode_name(CaptureRouteMode mode) noexcept
{
    switch (mode) {
    case CaptureRouteMode::FollowSystem:
        return "follow_system";
    case CaptureRouteMode::PreferredDevice:
        return "preferred_device";
    default:
        return "unknown";
    }
}

} // namespace aqua::audio

#endif // AQUA_AUDIO_CAPTURE_CAPTURE_ROUTE_MODE_H
