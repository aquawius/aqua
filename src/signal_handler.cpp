//
// Created by aquawius on 25-1-14.
//

#include "signal_handler.h"
#include <csignal>
#include <spdlog/spdlog.h>

signal_handler& signal_handler::get_instance()
{
    static signal_handler instance;
    return instance;
}

void signal_handler::setup()
{
    std::signal(SIGINT, signal_handler::handle_signal);
    // std::signal(SIGTERM, signal_handler::handle_signal);
    // std::signal(SIGABRT, signal_handler::handle_signal);

#ifndef _WIN32
// std::signal(SIGQUIT, signal_handler::handle_signal);
// std::signal(SIGHUP, signal_handler::handle_signal);
#endif

    spdlog::info("[signal_handler] Signal handler setup signals completed");
}

void signal_handler::register_callback(signal_callback callback)
{
    if (callback) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_callbacks.push_back(std::move(callback));
    }
}

void signal_handler::clear_callbacks()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_callbacks.clear();
}

void signal_handler::handle_signal(int signal)
{
    spdlog::info("[signal_handler] Received signal: {}", signal);
    auto& instance = get_instance();

    std::lock_guard<std::mutex> lock(instance.m_mutex);
    for (const auto& callback : instance.m_callbacks) {
        try {
            callback();
        } catch (const std::exception& e) {
            spdlog::error("[signal_handler] Error in signal callback: {}", e.what());
        }
    }
}
