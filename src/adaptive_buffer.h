//
// Created by aquawius on 25-2-2.
//

#ifndef ADAPTIVE_BUFFER_H
#define ADAPTIVE_BUFFER_H

#include <atomic>
#include <cstdint>
#include <map>
#include <vector>

class adaptive_buffer {
public:
    struct AudioPacketHeader {
        uint32_t sequence_number;
        uint64_t timestamp;
    } __attribute__((packed));

    adaptive_buffer();
    ~adaptive_buffer() = default;
    adaptive_buffer(const adaptive_buffer&) = delete;
    adaptive_buffer& operator=(const adaptive_buffer&) = delete;
    adaptive_buffer(adaptive_buffer&&) = delete;

    bool push_buffer_packets(std::vector<uint8_t>&& packet_with_header);
    size_t pull_buffer_data(float* output_buffer, size_t need_samples_size);

private:
    struct CompareSequenceNumber {
        bool operator()(uint32_t a, uint32_t b) const {
            return static_cast<int32_t>(a - b) < 0;
        }
    };

    static constexpr size_t MAX_ADAPTIVE_BUFFER_MAP_SIZE = 500; // 100包/s
    std::map<uint32_t, std::vector<uint8_t>, CompareSequenceNumber> m_main_packets_buffer;
    std::vector<uint8_t> m_last_pull_remains;

    std::atomic<uint32_t> m_pull_expected_seq; // pull指针
    std::atomic<uint32_t> m_push_base_seq { 0 }; // push基准指针
    std::atomic<bool> m_initialized { false }; // 缓冲区初始化标志

    int m_muted_count = 0;  // 或许能减少卡死几率
};

#endif // ADAPTIVE_BUFFER_H