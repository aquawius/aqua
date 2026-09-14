#ifndef AQUA_NET_NET_ERROR_H
#define AQUA_NET_NET_ERROR_H

// 网络层错误码。
//
// 背景：net 域此前一律用 bool 表达失败，调用方只能打一句笼统日志，无法区分
// "端口被占用" / "地址非法" / "已停止" / "地址族不一致"。与 audio 域的
// AudioError 对齐，统一改用 std::expected<T, NetError>：失败原因进入类型系统，
// 且 [[nodiscard]] 让"忽略返回值"成为显式行为。

#include <array>
#include <cstdint>
#include <string_view>
#include <utility>

namespace aqua::net {

enum class NetError : std::uint8_t {
    None = 0,
    Stopped, // transport 已停止
    AlreadyBound, // 已绑定，且请求的 endpoint 与当前不同（换地址族/端口需新建 transport）
    InvalidEndpoint, // IP 字面量非法，或端口为 0
    AddressFamilyMismatch, // 远端地址族与已打开 socket 不一致
    BindFailed, // 底层 socket open/bind 失败（端口占用、权限等，细节见日志）
    NotOpen, // socket 未打开（应先 bind/open 再 start_receive）
    ReceiveStartFailed, // 接收循环启动失败（strand 投递/首次 async_receive 异常）
    AlreadyStarted, // 数据面已启动，配置/启动请求被忽略（不可再改）
    InvalidArgument, // 调用参数本身不合法（payload 为 0 / handler 为空）
};

// 名字表长度 = 最后一个枚举成员 + 1：加枚举值而不同步 names 表 = 编译失败
// （此前只有运行期 "unknown" 兜底，诊断会静默降级）。
inline constexpr std::size_t kNetErrorCount = std::to_underlying(NetError::InvalidArgument) + 1;

[[nodiscard]] inline constexpr std::string_view net_error_name(NetError error) noexcept
{
    constexpr std::array<std::string_view, kNetErrorCount> names {
        "none",
        "stopped",
        "already_bound",
        "invalid_endpoint",
        "address_family_mismatch",
        "bind_failed",
        "not_open",
        "receive_start_failed",
        "already_started",
        "invalid_argument",
    };
    const auto idx = static_cast<std::size_t>(std::to_underlying(error));
    if (idx >= names.size()) {
        return "unknown";
    }
    return names[idx];
}

} // namespace aqua::net

#endif // AQUA_NET_NET_ERROR_H
