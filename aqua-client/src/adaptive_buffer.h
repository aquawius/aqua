//
// Created by aquawius on 25-2-2.
//

#ifndef ADAPTIVE_BUFFER_H
#define ADAPTIVE_BUFFER_H

#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>
#include <vector>

class adaptive_buffer {
public:
#ifdef _MSC_VER
#pragma pack(push, 1)
#endif

    // 音频包头结构
    struct AudioPacketHeader {
        uint32_t sequence_number; // 序列号
        uint64_t timestamp; // 时间戳
    }
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((packed))
#endif
    ;

#ifdef _MSC_VER
#pragma pack(pop)
#endif

    static_assert(sizeof(AudioPacketHeader) == sizeof(uint32_t) + sizeof(uint64_t), "AudioPacketHeader Size align ERROR!");

    adaptive_buffer();
    ~adaptive_buffer() = default;
    adaptive_buffer(const adaptive_buffer&) = delete;
    adaptive_buffer& operator=(const adaptive_buffer&) = delete;
    adaptive_buffer(adaptive_buffer&&) = delete;

    bool push_buffer_packets(std::vector<uint8_t>&& packet_with_header);
    size_t pull_buffer_data(float* output_buffer, size_t need_samples_size);

private:
    struct CompareSequenceNumber {
        bool operator()(uint32_t a, uint32_t b) const
        {
            return static_cast<int32_t>(a - b) < 0;
        }
    };

    static constexpr size_t MAX_ADAPTIVE_BUFFER_MAP_SIZE = 500; // 100包/s
    std::map<uint32_t, std::vector<uint8_t>, CompareSequenceNumber> m_main_packets_buffer;
    std::vector<uint8_t> m_last_pull_remains;

    mutable std::mutex m_main_buffer_mutex;

    std::atomic<uint32_t> m_pull_expected_seq; // pull指针
    std::atomic<uint32_t> m_push_base_seq { 0 }; // push基准指针
    std::atomic<bool> m_initialized { false }; // 缓冲区初始化标志

    static constexpr uint32_t MAX_ALLOWED_GAP = 10; // 允许跳包的间隙数
    std::vector<uint32_t> m_gap_history;
    int m_muted_count = 0; // 减少跳包几率

    std::vector<int64_t> m_latencies; // 存储延迟数据（毫秒）
};

#endif // ADAPTIVE_BUFFER_H