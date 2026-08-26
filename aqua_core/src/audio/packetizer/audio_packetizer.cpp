#include "aqua/audio/packetizer/audio_packetizer.h"

#include <new>

namespace aqua::audio {

AudioPacketizer::AudioPacketizer(std::uint32_t frame_count, std::uint32_t frame_bytes)
    : frame_bytes_(frame_bytes)
    , frame_count_(frame_count)
{
}

bool AudioPacketizer::is_valid_config(std::uint32_t frame_count, std::uint32_t frame_bytes) noexcept
{
    return frame_count != 0 && frame_bytes != 0;
}

bool AudioPacketizer::valid() const noexcept
{
    return is_valid_config(frame_count_, frame_bytes_);
}

void AudioPacketizer::push(std::span<const std::byte> pcm, FrameHandler& handler) noexcept
{
    const std::size_t chunk = static_cast<std::size_t>(frame_count_) * frame_bytes_;
    if (chunk == 0) {
        pending_.clear();
        return;
    }
    // 校验 frame-aligned：pcm 必须是 frame_bytes 的整数倍。非整帧输入会跨越
    // sample-frame 边界（后续输入无法安全补齐），直接丢弃。
    if (pcm.size() % frame_bytes_ != 0) {
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
        if (handler) {
            handler(sequence_, std::span<const std::byte>(pending_.data(), chunk));
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
