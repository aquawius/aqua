#include "aqua/audio/packetizer/audio_packetizer.h"

#include <new>

namespace aqua::audio {

AudioPacketizer::AudioPacketizer(std::uint32_t frame_count, std::uint32_t frame_bytes)
    : frame_bytes_(frame_bytes)
    , frame_count_(frame_count)
{
    // 预分配攒块缓冲：消除攒满一帧（chunk 字节）过程中的 vector realloc，
    // 避免在实时采集线程触发堆分配。超大单块输入首次仍可能扩容一次，之后容量稳定。
    const std::size_t chunk = static_cast<std::size_t>(frame_count_) * frame_bytes_;
    if (chunk != 0) {
        try {
            pending_.reserve(chunk);
        } catch (const std::bad_alloc&) {
            // 预分配失败不致命：后续 push 仍可工作（按需 realloc），仅失去预分配收益。
        }
    }
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
            handler(AudioFrame { .sequence = sequence_, .frame_count = frame_count_,
                .data = std::span<const std::byte>(pending_.data(), chunk) });
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
