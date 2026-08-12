#ifndef AQUA_SPSC_RING_BUFFER_H
#define AQUA_SPSC_RING_BUFFER_H

#include <atomic>
#include <cstddef>
#include <span>
#include <vector>

namespace aqua::audio {

// 单生产者单消费者无锁环形缓冲。
// 容量向上取整为 2 的幂，便于掩码取模。
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

    // 总容量（已取整为 2 的幂）。
    std::size_t capacity() const noexcept { return buffer_.size(); }

    // 清空缓冲。仅在两端都停止时调用。
    void clear() noexcept;

private:
    std::vector<std::byte> buffer_;
    const std::size_t mask_;

    // 写索引（仅生产者写，消费者读）
    std::atomic<std::size_t> write_pos_{0};
    // 读索引（仅消费者写，生产者读）
    std::atomic<std::size_t> read_pos_{0};
};

} // namespace aqua::audio

#endif // AQUA_SPSC_RING_BUFFER_H
