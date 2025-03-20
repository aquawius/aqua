//
// Created by QU on 25-2-11.
//

#include "audio_playback.h"

#if defined(_WIN32) || defined(_WIN64)
#include "windows/audio_playback_windows.h"
#elif defined(__linux__)
#include "linux/audio_playback_linux.h"
#endif

std::shared_ptr<audio_playback> audio_playback::create()
{
#if defined(_WIN32) || defined(_WIN64)
    return std::make_shared<audio_playback_windows>();
#elif defined(__linux__)
    return std::make_shared<audio_playback_linux>();
#else
    spdlog::error("Unsupported platform");
    return nullptr;
#endif
}