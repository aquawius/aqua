#include "aqua/audio/packetizer/audio_packetizer.h"

#include <new>

namespace aqua::audio {

AudioPacketizer::AudioPacketizer(std::uint32_t frame_count, std::uint32_t frame_bytes)
    : frame_bytes_(frame_bytes)
    , frame_count_(frame_count)
{
}

void AudioPacketizer::push(std::span<const std::byte> pcm, FrameHandler handler, void* user_data) noexcept
{
    const std::size_t chunk = static_cast<std::size_t>(frame_count_) * frame_bytes_;
    if (chunk == 0) {
        pending_.clear();
        return;
    }

    try {
        pending_.insert(pending_.end(), pcm.begin(), pcm.end());
    } catch (const std::bad_alloc&) {
        // 分配失败：丢弃余量，避免破坏帧对齐；实时路径不允许异常逃逸。
        pending_.clear();
        return;
    }

    while (pending_.size() >= chunk) {
        if (handler != nullptr) {
            handler(user_data, sequence_, std::span<const std::byte>(pending_.data(), chunk));
        }
        ++sequence_;
        // erase 仅移动元素（std::byte 平凡可析构），不分配、不抛异常。
        pending_.erase(pending_.begin(), pending_.begin() + static_cast<std::ptrdiff_t>(chunk));
    }
}

void AudioPacketizer::reset() noexcept
{
    pending_.clear();
    sequence_ = 0;
}

} // namespace aqua::audio
