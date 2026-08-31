#ifndef AQUA_COMPAT_MOVE_ONLY_FUNCTION_H
#define AQUA_COMPAT_MOVE_ONLY_FUNCTION_H

// std::move_only_function 的跨平台兼容层。
//
// 背景：C++23 P0288 move_only_function 在 MSVC / libstdc++ 13+ 可用，
// 但 NDK 的 libc++ 至今未实现（feature-test 宏缺席）。core 的回调类型
// （capture / playback / udp）需要同时编译到两端，因此统一经本别名声明。
//
// 回退语义（libc++ 无实现时降级为 std::function）：
//   - std::function 是可拷贝的；我们的回调均为一次性 set、从不拷贝，
//     行为等价，仅失去 move-only 编译期约束；
//   - 签名中的 noexcept 被剥除：std::function 的签名不接受 noexcept
//     （其部分特化只匹配 R(Args...)），存储的 noexcept callable 照常调用。
//
// 使用约束：经本别名声明的回调类型不得依赖"不可拷贝"这一性质
// （例如用拷贝构造做编译期防护），否则 MSVC / Android 行为分叉。

#include <functional>
#include <version>

namespace aqua::compat {

#if defined(__cpp_lib_move_only_function)

template <class Signature>
using MoveOnlyFunction = std::move_only_function<Signature>;

#else

namespace detail {

template <class Signature>
struct move_only_function_impl; // 未定义：只接受函数类型签名

template <class R, class... Args>
struct move_only_function_impl<R(Args...)> {
    using type = std::function<R(Args...)>;
};

// noexcept 签名：剥除 noexcept（见文件头注释）。
template <class R, class... Args>
struct move_only_function_impl<R(Args...) noexcept> {
    using type = std::function<R(Args...)>;
};

} // namespace detail

template <class Signature>
using MoveOnlyFunction = typename detail::move_only_function_impl<Signature>::type;

#endif

} // namespace aqua::compat

#endif // AQUA_COMPAT_MOVE_ONLY_FUNCTION_H
