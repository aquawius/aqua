#ifndef AQUA_SPSC_RING_BUFFER_H
#define AQUA_SPSC_RING_BUFFER_H

#include <atomic>
#include <cstddef>
#include <span>
#include <vector>

/**
 * SPSC 环形缓冲区（Single Producer, Single Consumer）是一种无锁队列，基于环形缓冲区实现。
 * 其核心原理是通过写指针（write_pos）和读指针（read_pos）形成循环结构，确保数据的顺序性和可见性。
 * 不需要复杂的同步机制，只需确保内存操作的可见性和顺序性即可实现线程安全。
 */

namespace aqua::audio {

// 单生产者单消费者无锁环形缓冲。
// 容量向上取整为 1KiB（1024 字节）的倍数，避免 2 的幂取整的过度分配。
// 仅允许 1 写 1 读；多生产者/消费者场景需外层串行化。
// 写满返回实际写入量（不阻塞、不覆盖未读数据），调用方负责统计丢弃。
class SpscRingBuffer {
public:
    explicit SpscRingBuffer(std::size_t capacity_bytes);

    SpscRingBuffer(const SpscRingBuffer&) = delete;
    SpscRingBuffer& operator=(const SpscRingBuffer&) = delete;
    SpscRingBuffer(SpscRingBuffer&&) = delete;
    SpscRingBuffer& operator=(SpscRingBuffer&&) = delete;

    // 返回实际写入字节数（可能小于请求值，若缓冲接近写满）。
    std::size_t write(std::span<const std::byte> data) noexcept;

    // 返回实际读出字节数。
    std::size_t read(std::span<std::byte> out) noexcept;

    // 可读字节数（消费者侧调用）。
    std::size_t available_read() const noexcept;

    // 可写字节数（生产者侧调用）。
    std::size_t available_write() const noexcept;

    // 总容量（已向上取整为 1KiB 的倍数）。
    std::size_t capacity() const noexcept;

    // 清空缓冲。仅在两端都停止时调用。
    void clear() noexcept;

private:
    std::vector<std::byte> buffer_;

    // 写索引（仅生产者写，消费者读）
    // cache line 对齐避免 false sharing
    alignas(64) std::atomic<std::size_t> write_pos_ { 0 };
    // 读索引（仅消费者写，生产者读）
    alignas(64) std::atomic<std::size_t> read_pos_ { 0 };
};

} // namespace aqua::audio

#endif // AQUA_SPSC_RING_BUFFER_H
