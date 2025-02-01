//
// Created by aquawius on 25-2-2.
//

#ifndef ADAPTIVE_BUFFER_H
#define ADAPTIVE_BUFFER_H

#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <vector>

class adaptive_buffer {

public:
    // 音频包头结构
    struct AudioPacketHeader {
        uint32_t sequence_number; // 序列号
        uint64_t timestamp;       // 时间戳
    } __attribute__((packed));

    adaptive_buffer();
    ~adaptive_buffer() = default;
    adaptive_buffer(const adaptive_buffer&) = delete;
    adaptive_buffer& operator=(const adaptive_buffer&) = delete;
    adaptive_buffer(adaptive_buffer&&) = delete;

    bool put_buffer_packets(std::vector<uint8_t>&& packet_with_header);
    size_t get_samples(float* output_buffer, size_t need_samples);

private:
    static constexpr size_t MAX_BUFFERED_SAMPLES = 48000 * 2 * 3; // 缓冲最多5秒的音频数据（48000Hz，立体声）
    std::deque<std::vector<uint8_t>> m_packet_queue;
    size_t m_total_samples_in_buffer{0};

    std::vector<uint8_t> last_get_remains;
};

#endif // ADAPTIVE_BUFFER_H
