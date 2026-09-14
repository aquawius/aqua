#ifndef AQUA_AUDIO_FILL_SILENCE_H
#define AQUA_AUDIO_FILL_SILENCE_H

// 播放后端共用的静音填充（RT 安全：无分配、无锁、无系统调用）。

#include <algorithm>
#include <cstddef>
#include <span>

namespace aqua::audio {

// 输出缓冲的未写入尾部补静音：回放回调契约要求未填满部分不得残留上一批
// 数据（audio_playback.h 头注释）。WASAPI 与 AAudio 后端此前各写一遍。
// written_bytes >= output.size() 时为 no-op。
inline void fill_silence_tail(std::span<std::byte> output, std::size_t written_bytes,
    std::byte silence) noexcept
{
    if (written_bytes < output.size()) {
        std::ranges::fill(output.subspan(written_bytes), silence);
    }
}

} // namespace aqua::audio

#endif // AQUA_AUDIO_FILL_SILENCE_H
