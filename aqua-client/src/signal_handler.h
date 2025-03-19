//
// Created by aquawius on 25-1-14.
//

#ifndef SIGNAL_HANDLER_H
#define SIGNAL_HANDLER_H

#include <atomic>
#include <functional>
#include <mutex>
#include <vector>

class signal_handler {
public:
    using signal_callback = std::function<void()>;

    static signal_handler& get_instance();

    // 设置信号处理器，只处理 SIGINT
    void setup();

    // 注册回调函数
    void register_callback(signal_callback callback);

    // 清除所有回调
    void clear_callbacks();

    // 检查是否正在处理信号
    bool is_handling_signal() const { return m_is_handling_signal.load(); }

private:
    signal_handler() = default;
    ~signal_handler() = default;
    signal_handler(const signal_handler&) = delete;
    signal_handler& operator=(const signal_handler&) = delete;

    // 静态信号处理函数
    static void handle_signal(int signal);

    // 回调函数列表
    std::vector<signal_callback> m_callbacks;

    // 保护回调列表的互斥锁
    mutable std::mutex m_mutex;

    // 标记是否正在处理信号
    std::atomic<bool> m_is_handling_signal { false };
};

#endif // SIGNAL_HANDLER_H