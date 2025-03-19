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
    // 只注册 SIGINT 信号处理
    if (std::signal(SIGINT, signal_handler::handle_signal) == SIG_ERR) {
        spdlog::error("[signal_handler] Failed to register SIGINT handler");
        return;
    }

    spdlog::info("[signal_handler] SIGINT handler registered successfully");
}

void signal_handler::register_callback(signal_callback callback)
{
    if (!callback) {
        spdlog::warn("[signal_handler] Attempted to register null callback");
        return;
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    m_callbacks.push_back(std::move(callback));
    spdlog::debug("[signal_handler] New callback registered, total callbacks: {}", m_callbacks.size());
}

void signal_handler::clear_callbacks()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_callbacks.clear();
    spdlog::debug("[signal_handler] All callbacks cleared");
}

void signal_handler::handle_signal(int signal)
{
    // 只处理 SIGINT
    if (signal != SIGINT) {
        spdlog::warn("[signal_handler] Received unexpected signal: {}", signal);
        return;
    }

    auto& instance = get_instance();
    
    // 检查是否已经在处理信号
    if (instance.m_is_handling_signal.exchange(true)) {
        spdlog::warn("[signal_handler] Signal handling already in progress, ignoring duplicate signal");
        return;
    }

    spdlog::info("[signal_handler] Processing SIGINT signal...");

    // 使用 RAII 锁保护回调执行
    {
        std::lock_guard<std::mutex> lock(instance.m_mutex);
        
        // 执行所有注册的回调
        for (const auto& callback : instance.m_callbacks) {
            try {
                callback();
            } catch (const std::exception& e) {
                spdlog::error("[signal_handler] Error in signal callback: {}", e.what());
            }
        }
    }

    // 重置信号处理状态
    instance.m_is_handling_signal.store(false);
    spdlog::info("[signal_handler] Signal handling completed");
}