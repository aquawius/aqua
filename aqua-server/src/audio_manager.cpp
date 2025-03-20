//
// Created by QU on 25-2-10.
//

#include "audio_manager.h"

#if defined(_WIN32) || defined(_WIN64)
#include "windows/audio_manager_impl_windows.h"
#elif defined(__linux__)
#include "linux/audio_manager_impl_linux.h"
#endif

std::shared_ptr<audio_manager> audio_manager::create()
{
#if defined(_WIN32) || defined(_WIN64)
    return std::make_shared<audio_manager_impl_windows>();
#elif defined(__linux__)
    return std::make_shared<audio_manager_impl_linux>();
#else
    spdlog::error("Unsupported platform");
    return nullptr;
#endif
}