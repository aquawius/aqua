#include "core/audio/ringbuffer/spsc_ringbuffer.h"

#include <algorithm>
#include <stdexcept>

namespace aqua::audio {

namespace {
    // 向上取整为 2 的幂，最小 64 字节。
    std::size_t round_up_pow2(std::size_t v)
    {
        if (v < 64) return 64;
        --v;
        v |= v >> 1;
        v |= v >> 2;
        v |= v >> 4;
        v |= v >> 8;
        v |= v >> 16;
        v |= v >> 32;
        return v + 1;
    }
} // namespace

SpscRingBuffer::SpscRingBuffer(std::size_t capacity_bytes)
    : buffer_(round_up_pow2(capacity_bytes))
    , mask_(buffer_.size() - 1)
{
}

std::size_t SpscRingBuffer::write(std::span<const std::byte> data) noexcept
{
    const std::size_t w = write_pos_.load(std::memory_order_relaxed);
    const std::size_t r = read_pos_.load(std::memory_order_acquire);
    const std::size_t free_space = buffer_.size() - (w - r);
    const std::size_t to_write = std::min(data.size(), free_space);

    // 可能跨尾部，分两段拷贝
    const std::size_t idx = w & mask_;
    const std::size_t first = std::min(to_write, buffer_.size() - idx);
    std::copy_n(data.data(), first, buffer_.data() + idx);
    if (to_write > first) {
        std::copy_n(data.data() + first, to_write - first, buffer_.data());
    }

    write_pos_.store(w + to_write, std::memory_order_release);
    return to_write;
}

std::size_t SpscRingBuffer::read(std::span<std::byte> out) noexcept
{
    const std::size_t r = read_pos_.load(std::memory_order_relaxed);
    const std::size_t w = write_pos_.load(std::memory_order_acquire);
    const std::size_t available = w - r;
    const std::size_t to_read = std::min(out.size(), available);

    const std::size_t idx = r & mask_;
    const std::size_t first = std::min(to_read, buffer_.size() - idx);
    std::copy_n(buffer_.data() + idx, first, out.data());
    if (to_read > first) {
        std::copy_n(buffer_.data(), to_read - first, out.data() + first);
    }

    read_pos_.store(r + to_read, std::memory_order_release);
    return to_read;
}

std::size_t SpscRingBuffer::available_read() const noexcept
{
    return write_pos_.load(std::memory_order_acquire) -
           read_pos_.load(std::memory_order_relaxed);
}

std::size_t SpscRingBuffer::available_write() const noexcept
{
    return buffer_.size() - available_read();
}

void SpscRingBuffer::clear() noexcept
{
    write_pos_.store(0, std::memory_order_relaxed);
    read_pos_.store(0, std::memory_order_relaxed);
}

} // namespace aqua::audio
