#ifndef AQUA_AUDIO_DEVICES_AUDIO_ROUTE_H
#define AQUA_AUDIO_DEVICES_AUDIO_ROUTE_H

// 路由的第一根轴：这条流的路由权归谁。
//
// 原先用 std::optional<AudioDeviceId> 表达设备选择，一个字段承担两种语义
// ——「目标端点是哪个」与「谁拥有路由控制权」。这在 Android 上表现为一个
// 看起来像 bug 的矛盾：跟随系统时系统控制栏能改路由（AAudio 不调
// setDeviceId，平台持有控制权），App 内手选设备后控制栏就改不动了
// （setDeviceId 把流绑死，Aqua 拿走了控制权）。两种行为都是对的，但
// 「id 是否为空」这个表达说不清它们为什么不同。
//
// 显式化之后，各平台的映射是同一套：
//   System       -> AAudio 不调 setDeviceId / WASAPI 用默认 endpoint /
//                   PipeWire 交 session manager 路由 / Core Audio 用默认 device
//   Application  -> AAudio setDeviceId / WASAPI 显式 endpoint /
//                   PipeWire 强制 target.node / Core Audio 指定 device
// 因此「系统默认输出变了，但一条已绑定到具体端点的流不跟着变」在所有平台
// 都成立，不是 Android 特例。
//
// 设备 id 本身是平台/会话内的实现细节（跨会话不稳定、不可比较），所以它
// 只作为 Application 权威下的载荷出现，不是独立的请求维度。
//
// 这里只表达「请求」。绑定失败时降级还是 Fatal 属于策略，见 route_policy.h。

#include "aqua/audio/devices/audio_device.h"

#include <optional>
#include <utility>

namespace aqua::audio {

enum class AudioRouteAuthority : unsigned char {
    System, // 由平台选择端点，并保留后续变更端点的权力。
    Application, // 把这条流绑定到 route.device。
};

struct AudioRoute {
    AudioRouteAuthority authority = AudioRouteAuthority::System;
    std::optional<AudioDeviceId> device;

    [[nodiscard]] static constexpr AudioRoute follow_system() noexcept
    {
        return { AudioRouteAuthority::System, std::nullopt };
    }

    [[nodiscard]] static AudioRoute pin(AudioDeviceId id)
    {
        return { AudioRouteAuthority::Application, std::move(id) };
    }

    // 从可空的旧式请求构造（CLI / C API 边界用）：空即跟随系统。
    [[nodiscard]] static AudioRoute from_optional(std::optional<AudioDeviceId> id)
    {
        return id.has_value() && !id->empty()
            ? pin(std::move(*id))
            : follow_system();
    }

    [[nodiscard]] bool follows_system() const noexcept
    {
        return authority == AudioRouteAuthority::System;
    }

    [[nodiscard]] bool is_pinned() const noexcept
    {
        return authority == AudioRouteAuthority::Application
            && device.has_value() && !device->empty();
    }

    // 交给 backend 的端点请求。跟随系统时**刻意**不带 id：这正是让 Android
    // 的输出切换器、PipeWire 的 session manager、各平台的默认设备策略保有
    // 路由权的原因。
    [[nodiscard]] std::optional<AudioDeviceId> endpoint_request() const
    {
        return is_pinned() ? device : std::nullopt;
    }
};

[[nodiscard]] constexpr const char* audio_route_authority_name(
    AudioRouteAuthority authority) noexcept
{
    switch (authority) {
    case AudioRouteAuthority::System:
        return "system";
    case AudioRouteAuthority::Application:
        return "application";
    }
    return "unknown";
}

} // namespace aqua::audio

#endif // AQUA_AUDIO_DEVICES_AUDIO_ROUTE_H
