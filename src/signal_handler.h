//
// Created by aquawius on 25-1-14.
//

#ifndef SIGNAL_HANDLER_H
#define SIGNAL_HANDLER_H

#include <functional>
#include <vector>
#include <mutex>

class signal_handler {
public:
    using signal_callback = std::function<void()>;

    static signal_handler& get_instance();

    void setup();
    void register_callback(signal_callback callback);
    void clear_callbacks();

private:
    signal_handler() = default;
    ~signal_handler() = default;
    signal_handler(const signal_handler&) = delete;
    signal_handler& operator=(const signal_handler&) = delete;

    static void handle_signal(int signal);

    std::vector<signal_callback> m_callbacks;
    std::mutex m_mutex; // 保护回调列表
};

#endif // SIGNAL_HANDLER_H