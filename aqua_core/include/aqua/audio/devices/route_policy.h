#ifndef AQUA_AUDIO_DEVICES_ROUTE_POLICY_H
#define AQUA_AUDIO_DEVICES_ROUTE_POLICY_H

// 路由的第二根轴。
//
// AudioRoute（audio_route.h）回答「这条流绑到哪个端点、路由权归谁」，是给
// backend 的请求。RoutePolicy 回答「绑定失败时怎么办、这个意图归谁」，是
// manager 的决策状态。两者正交：同一条 pin 到蓝牙的流，server 侧要求
// 丢失即 Fatal（静默换源会让采集内容与预期不符且无从告知），client 侧要求
// 降级到系统默认（移动端永不主动静音）。只用 AudioRoute 表达不了这个差别。
//
// 这套词汇取代原先并行的 PlaybackRouteMode / CaptureRouteMode：那两组枚举
// 各自把「意图归属」与「丢失动作」压扁成一个值，导致 playback 的
// PreferredDevice（优先+降级）与 capture 的 PreferredDevice（钉住+Fatal）
// 同名不同义。拆成两个轴后，四种既有语义都能表达，且 WASAPI / PipeWire /
// Core Audio / AAudio 共用同一套。
//
// 与既有三模式的对应关系（诊断投影，见 label()/diagnostic_code()）：
//   owner=None         -> follow_system     （两端同名同义）
//   owner=Application  -> prefer_current    （仅 playback：App 钉住首流实际落点）
//   owner=User         -> preferred_device  （playback=优先+降级，capture=钉住+Fatal）

#include "aqua/audio/devices/audio_device.h"
#include "aqua/audio/devices/audio_route.h"

#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>

namespace aqua::audio {

// 意图归属：决定 sticky 语义与设备回归时是否自动切回。
//
// 底层取值即对外的诊断编码（aqua_route_mode / Kotlin AquaRouteMode 按 code
// 镜像），不得重排；aqua_capi.cpp 以 static_assert 锁定。
enum class RouteIntentOwner : std::uint8_t {
    None = 0, // 无钉住意图，跟随系统默认。-> AQUA_ROUTE_FOLLOW_SYSTEM
    Application = 1, // App 自动钉住首流实际落点；非 sticky，回归不自动切回。
    // -> AQUA_ROUTE_PREFER_CURRENT
    User = 2, // 用户手选或启动配置指定；sticky，设备回归自动切回。
    // -> AQUA_ROUTE_PREFERRED_DEVICE
};

// 诊断标签（稳定字符串，进 snapshot 文本视图）。
[[nodiscard]] constexpr std::string_view route_intent_owner_label(
    RouteIntentOwner owner) noexcept
{
    switch (owner) {
    case RouteIntentOwner::None:
        return "follow_system";
    case RouteIntentOwner::Application:
        return "prefer_current";
    case RouteIntentOwner::User:
        return "preferred_device";
    }
    return "unknown";
}

// 钉住的设备不可用时的动作。两端取舍相反且必须相反（见文件头）。
enum class RouteLossAction : std::uint8_t {
    FallbackToSystem, // 按候选链降级到系统默认，保住会话。
    Fatal, // 绝不降级，进入 Fatal 终态由决策者终止会话。
};

struct RoutePolicy {
    RouteIntentOwner owner = RouteIntentOwner::None;
    RouteLossAction on_loss = RouteLossAction::FallbackToSystem;
    // sticky 意图目标。owner != None 时有意义；fallback 降级不覆盖它——
    // 这是「优先而非固定」语义的载体，也是自动切回与错误驱动 restart 的
    // 目标来源。
    std::optional<AudioDeviceId> intent;

    [[nodiscard]] static constexpr RoutePolicy follow_system(
        RouteLossAction loss = RouteLossAction::FallbackToSystem) noexcept
    {
        return { RouteIntentOwner::None, loss, std::nullopt };
    }

    // App 自动钉住（playback 的「自动切换播放设备」关）。intent 由调用方在
    // 首流成功、读到实际落点后填入；读不到时保持 None 语义退化。
    [[nodiscard]] static RoutePolicy app_pinned(AudioDeviceId id,
        RouteLossAction loss = RouteLossAction::FallbackToSystem)
    {
        return { RouteIntentOwner::Application, loss, std::move(id) };
    }

    [[nodiscard]] static RoutePolicy user_pinned(AudioDeviceId id,
        RouteLossAction loss = RouteLossAction::FallbackToSystem)
    {
        return { RouteIntentOwner::User, loss, std::move(id) };
    }

    // 由 backend 请求推导初始 policy。app_pin_on_system_route 表达
    // prefer_current_on_start：请求跟随系统，但首流落点要被 App 钉住。
    // 此时 intent 尚未可知，owner 先记 None，由 manager 在读到实际落点后
    // 升级为 app_pinned。
    [[nodiscard]] static RoutePolicy from_route(const AudioRoute& route,
        RouteLossAction loss)
    {
        return route.is_pinned()
            ? user_pinned(*route.device, loss)
            : follow_system(loss);
    }

    [[nodiscard]] bool follows_system() const noexcept
    {
        return owner == RouteIntentOwner::None;
    }

    // 用户意图才会在设备回归时自动切回；App 自动钉住不会（它本来就只是
    // 「别乱跑」，不是「我认准了这个」）。
    [[nodiscard]] bool auto_returns() const noexcept
    {
        return owner == RouteIntentOwner::User;
    }

    [[nodiscard]] bool fatal_on_loss() const noexcept
    {
        return on_loss == RouteLossAction::Fatal;
    }

    // 错误驱动 restart 的目标推导。actual 是成功 start 时缓存的实际落点：
    // App 自动钉住锚定它，用户意图锚定 sticky intent，跟随系统则交给平台。
    [[nodiscard]] std::optional<AudioDeviceId> restart_target(
        const std::optional<AudioDeviceId>& actual) const
    {
        switch (owner) {
        case RouteIntentOwner::None:
            return std::nullopt;
        case RouteIntentOwner::Application:
            return intent.has_value() ? intent : actual;
        case RouteIntentOwner::User:
            return intent;
        }
        return std::nullopt;
    }

    // ---- 诊断投影 ----
    // label 与 diagnostic_code 是对外契约（见 RouteIntentOwner 的取值说明），
    // 不得变更。
    [[nodiscard]] std::string_view label() const noexcept
    {
        return route_intent_owner_label(owner);
    }

    [[nodiscard]] std::int32_t diagnostic_code() const noexcept
    {
        return static_cast<std::int32_t>(owner);
    }
};

[[nodiscard]] constexpr std::string_view route_loss_action_name(
    RouteLossAction action) noexcept
{
    switch (action) {
    case RouteLossAction::FallbackToSystem:
        return "fallback_to_system";
    case RouteLossAction::Fatal:
        return "fatal";
    }
    return "unknown";
}

} // namespace aqua::audio

#endif // AQUA_AUDIO_DEVICES_ROUTE_POLICY_H
