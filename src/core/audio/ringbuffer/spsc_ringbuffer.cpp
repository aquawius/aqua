#include "core/audio/ringbuffer/spsc_ringbuffer.h"
#include "core/public/config.h"

#include <algorithm>

namespace aqua::audio {

SpscRingBuffer::SpscRingBuffer(std::size_t capacity_bytes)
    : buffer_([&] {
        if (capacity_bytes < aqua::config::RINGBUFFER_MIN_BYTES)
            capacity_bytes = aqua::config::RINGBUFFER_MIN_BYTES;
        // 向上取整为 RINGBUFFER_ALIGNMENT_BYTES（1KiB）的倍数：8000 -> 8192, 10000 -> 10240。
        // 相比 2 的幂取整（10000 -> 16384），过度分配更少。
        const std::size_t align = aqua::config::RINGBUFFER_ALIGNMENT_BYTES;
        return (capacity_bytes + align - 1) / align * align;
    }())
{
}

std::size_t SpscRingBuffer::write(std::span<const std::byte> data) noexcept
{
    const std::size_t w = write_pos_.load(std::memory_order_relaxed);
    const std::size_t r = read_pos_.load(std::memory_order_acquire);
    const std::size_t free_space = buffer_.size() - (w - r);
    const std::size_t to_write = std::min(data.size(), free_space);

    // 可能跨尾部，分两段拷贝
    const std::size_t idx = w % buffer_.size();
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

    const std::size_t idx = r % buffer_.size();
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
    // 消费者视角：read_pos 是自己的（relaxed），write_pos 需 acquire 同步生产者的 release。
    return write_pos_.load(std::memory_order_acquire) - read_pos_.load(std::memory_order_relaxed);
}

std::size_t SpscRingBuffer::available_write() const noexcept
{
    // 生产者视角：write_pos 是自己的（relaxed），read_pos 需 acquire 同步消费者的 release。
    const std::size_t w = write_pos_.load(std::memory_order_relaxed);
    const std::size_t r = read_pos_.load(std::memory_order_acquire);
    return buffer_.size() - (w - r);
}

std::size_t SpscRingBuffer::capacity() const noexcept { return buffer_.size(); }

void SpscRingBuffer::clear() noexcept
{
    // 消费者排空：读指针追上写指针快照。只要求"仅消费者调用"（比"双端停止"更弱），
    // 生产者无需协调；clear 后生产者的后续写入基于其自己的 write_pos 继续，不变量保持。
    const std::size_t w = write_pos_.load(std::memory_order_acquire);
    read_pos_.store(w, std::memory_order_release);
}

} // namespace aqua::audio
