//
// Created by aquawius on 25-2-2.
//

#include "adaptive_buffer.h"
#include <boost/endian/conversion.hpp>
#include <cstring>
#include <numeric>
#include <spdlog/spdlog.h>

bool is_sequence_older(const uint32_t a, const uint32_t b)
{
    return static_cast<int32_t>(a - b) < 0;
}

adaptive_buffer::adaptive_buffer()
    : m_pull_expected_seq(0) {}

bool adaptive_buffer::push_buffer_packets(std::vector<std::byte>&& packet_with_header)
{
    if (packet_with_header.size() < sizeof(AudioPacketHeader)) {
        spdlog::warn("Invalid packet size: {}", packet_with_header.size());
        return false;
    }

    std::lock_guard<std::mutex> lock(m_main_buffer_mutex); // lock

    // 解析header
    AudioPacketHeader header { };
    std::memcpy(&header, packet_with_header.data(), sizeof(header));
    const uint32_t sequence_number = boost::endian::big_to_native(header.sequence_number);

    // 首次初始化基准序列号
    if (!m_initialized) {
        m_push_base_seq = sequence_number;
        m_pull_expected_seq = sequence_number;
        m_initialized.store(true, std::memory_order_relaxed);
        spdlog::trace("[PUSH] INIT  \t| base_seq:{}, next_seq:{} (new packet:{})",
            m_push_base_seq.load(), m_pull_expected_seq.load(), sequence_number);
    }

    // 之前的包
    if (is_sequence_older(sequence_number, m_pull_expected_seq)) {
        spdlog::warn("[PUSH] EXPIRED\t| seq={} (pull_seq={})",
            sequence_number, m_pull_expected_seq.load());
        return false;
    }

    // 检查重复包
    if (m_main_packets_buffer.contains(sequence_number)) {
        spdlog::warn("[PUSH] DUP   \t| seq={} (base:{})", sequence_number, m_push_base_seq.load());
        return false;
    }

    // 插入新包
    m_main_packets_buffer.emplace(sequence_number, std::move(packet_with_header));

    /*
    if (spdlog::get_level() <= spdlog::level::trace) {
        spdlog::trace("[PUSH] STORED\t| seq={} (buffer size:{})", sequence_number, m_main_packets_buffer.size());
    }
    */

    // 动态更新基准序列号（仅在包未被pull处理且比当前基准旧时更新）
    if (is_sequence_older(sequence_number, m_push_base_seq) && !is_sequence_older(sequence_number, m_pull_expected_seq)) {
        const uint32_t old_base = m_push_base_seq.load();
        m_push_base_seq.store(sequence_number, std::memory_order_relaxed);
        spdlog::info("[PUSH] BASE  \t| updated from {} to {} (new packet:{})",
            old_base, m_push_base_seq.load(), sequence_number);
    }

    // 容量管理：删除旧包时确保base_seq有效性
    while (m_main_packets_buffer.size() > MAX_ADAPTIVE_BUFFER_MAP_SIZE) {
        const auto oldest_it = m_main_packets_buffer.begin();
        const uint32_t erased_seq = oldest_it->first;
        m_main_packets_buffer.erase(oldest_it);
        spdlog::trace("[PUSH] PURGE \t| seq={} (buffer size:{})", erased_seq, m_main_packets_buffer.size());

        if (erased_seq == m_push_base_seq.load(std::memory_order_relaxed)) {
            if (auto new_base_it = m_main_packets_buffer.begin(); new_base_it != m_main_packets_buffer.end()) {
                m_push_base_seq.store(new_base_it->first, std::memory_order_relaxed);

                spdlog::info("[PUSH] BASE  \t| auto updated to {} after purge", new_base_it->first);
            } else {
                m_push_base_seq.store(0, std::memory_order_relaxed);
                spdlog::warn("[PUSH] RESET \t| buffer emptied, base_seq reset to 0");
            }
        }
    }

    return true;
}

size_t adaptive_buffer::pull_buffer_data(uint8_t* output_buffer, size_t need_bytes_size)
{
    if (!output_buffer || need_bytes_size == 0) {
        spdlog::warn("[PULL] INVALID\t| output buffer");
        return 0;
    }

    std::lock_guard<std::mutex> lock(m_main_buffer_mutex); // lock

    size_t filled_bytes = 0;

    if (!m_initialized) {
        // 填充静音
        std::memset(output_buffer, 0, need_bytes_size);
        return need_bytes_size;
    }

    // 处理残余数据
    if (!m_last_pull_remains.empty()) {
        const size_t copy_bytes = std::min(m_last_pull_remains.size(), need_bytes_size);

        std::memcpy(output_buffer, m_last_pull_remains.data(), copy_bytes);
        filled_bytes += copy_bytes;
        spdlog::trace("[PULL] REMAIN\t| used {} bytes (remaining:{})",
            copy_bytes, m_last_pull_remains.size() - copy_bytes);

        if (copy_bytes < m_last_pull_remains.size()) {
            m_last_pull_remains.erase(m_last_pull_remains.begin(),
                m_last_pull_remains.begin() + copy_bytes);
        } else {
            m_last_pull_remains.clear();
        }
    }

    uint32_t current_expected_seq = m_pull_expected_seq.load(std::memory_order_acquire);
    const uint32_t base_seq = m_push_base_seq.load(std::memory_order_relaxed);

    // 对齐基准序列号（处理pull滞后）
    if (is_sequence_older(current_expected_seq, base_seq)) {
        spdlog::warn("[PULL] SYNC  \t| jump from {} to base_seq:{}", current_expected_seq, base_seq);
        current_expected_seq = base_seq;
        m_pull_expected_seq.store(current_expected_seq, std::memory_order_release);
    }

    // 主数据填充循环
    while (filled_bytes < need_bytes_size) {
        auto it = m_main_packets_buffer.find(current_expected_seq);
        // 找到了current_expected_seq位置的包
        if (it != m_main_packets_buffer.end()) {
            // 验证部分
            const auto& packet = it->second;
            if (packet.size() < sizeof(AudioPacketHeader)) {
                spdlog::warn("[PULL] Corrupted packet at seq={}, size={}", current_expected_seq, packet.size());
                m_main_packets_buffer.erase(it);
                current_expected_seq++;
                continue;
            }

            // 解析音频包头
            AudioPacketHeader header { };
            std::memcpy(&header, packet.data(), sizeof(AudioPacketHeader));
            header.sequence_number = boost::endian::big_to_native(header.sequence_number);
            header.timestamp = boost::endian::big_to_native(header.timestamp);

            // 计算当前时间戳（毫秒）
            auto current_time = std::chrono::system_clock::now();
            const uint64_t current_timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    current_time.time_since_epoch())
                .count();

            // 计算延迟（注意处理负数情况）
            int64_t latency_ms = static_cast<int64_t>(current_timestamp_ms) - header.timestamp;

            // 记录延迟
            m_latencies.push_back(latency_ms);

            // 每1000个包计算平均延迟
            if (m_latencies.size() >= 1000) {
                int64_t sum = 0;
                for (auto lat : m_latencies)
                    sum += lat;
                int64_t avg = sum / m_latencies.size();
                spdlog::debug("[PULL] Average latency: {} ms (over {} packets)", avg, m_latencies.size());
                m_latencies.clear(); // 清空以备下次统计
            }

            // 开始处理包数据
            const std::byte* packet_data = packet.data() + sizeof(AudioPacketHeader);
            const size_t packet_bytes = packet.size() - sizeof(AudioPacketHeader);

            // 正常处理有效包
            const size_t remaining_need = need_bytes_size - filled_bytes;
            const size_t copy_bytes = std::min(packet_bytes, remaining_need);

            std::memcpy(output_buffer + filled_bytes, packet_data, copy_bytes);
            filled_bytes += copy_bytes;

            // 处理包内残余数据
            if (copy_bytes < packet_bytes) {
                const size_t remains_bytes = packet_bytes - copy_bytes;
                m_last_pull_remains.assign(
                    packet_data + copy_bytes,
                    packet_data + copy_bytes + remains_bytes);
                spdlog::trace("[PULL] SPLIT \t| seq={} (copied:{}, remains:{})",
                    current_expected_seq, copy_bytes, remains_bytes);
            }

            // 处理完了，删除
            m_main_packets_buffer.erase(it);
            /*
            spdlog::trace("[PULL] EXPIRE ERASE  | seq={} (buffer size:{})",
                current_expected_seq, m_main_packets_buffer.size());
            */
            current_expected_seq = (current_expected_seq == std::numeric_limits<uint32_t>::max()) ? 0 : current_expected_seq + 1;

        } else {
            // current_expected_seq位置的包没有

            // 动态跳跃逻辑 仅在丢失包的间隙超过阈值时跳转，减少小间隙的丢包
            auto next_it = m_main_packets_buffer.lower_bound(current_expected_seq); // 返回指向首个不小于给定键的元素的迭代器
            if (next_it != m_main_packets_buffer.end()) {
                // 找到了
                const uint32_t gap = (next_it->first >= current_expected_seq)
                    ? (next_it->first - current_expected_seq)
                    : (std::numeric_limits<uint32_t>::max() - current_expected_seq + next_it->first + 1);

                if (gap > MAX_ALLOWED_GAP) {
                    spdlog::warn("[PULL] JUMP  \t| from {} to {} (gap:{})",
                        current_expected_seq, next_it->first, gap);
                    current_expected_seq = next_it->first;
                    continue;
                } else {
                    // 小间隙不跳转，填充静音并逐步推进
                    spdlog::debug("[PULL] JUMP  \t| gap:{}, filling silence", gap);
                }
            }

            // 静音填充（使用零字节填充）
            const size_t silence_bytes = need_bytes_size - filled_bytes;
            std::memset(output_buffer + filled_bytes, 0, silence_bytes);
            filled_bytes += silence_bytes;
            spdlog::warn("[PULL] GAP   \t| filled {} silence bytes at seq={}", silence_bytes, current_expected_seq);

            // 略微增加一下, 在丢包的时候，别落下的太多
            if (++m_muted_count % 2 == 0) {
                current_expected_seq = (current_expected_seq == std::numeric_limits<uint32_t>::max()) ? 0 : current_expected_seq + 1;
            }

            break;
        }
    }

    m_pull_expected_seq.store(current_expected_seq, std::memory_order_release);

    /*
    if (spdlog::get_level() <= spdlog::level::trace) {
        spdlog::trace("[PULL] FINISH\t| filled {}/{} (next_seq:{}) (buffer size:{})",
            filled_bytes, need_bytes_size, current_expected_seq, m_main_packets_buffer.size());
    }
    */

    return filled_bytes;
}