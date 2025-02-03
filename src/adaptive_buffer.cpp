//
// Created by aquawius on 25-2-2.
//

#include "adaptive_buffer.h"
#include <boost/endian/conversion.hpp>
#include <cstring>
#include <spdlog/spdlog.h>

bool is_sequence_older(uint32_t a, uint32_t b)
{
    return static_cast<int32_t>(a - b) < 0;
}

adaptive_buffer::adaptive_buffer()
    : m_pull_expected_seq(0)
{
}

bool adaptive_buffer::push_buffer_packets(std::vector<uint8_t>&& packet_with_header)
{
    if (packet_with_header.size() < sizeof(AudioPacketHeader)) {
        spdlog::warn("Invalid packet size: {}", packet_with_header.size());
        return false;
    }

    // 解析header
    AudioPacketHeader header {};
    std::memcpy(&header, packet_with_header.data(), sizeof(header));
    const uint32_t sequence_number = boost::endian::big_to_native(header.sequence_number);

    // 首次初始化基准序列号
    if (!m_initialized) {
        m_push_base_seq = sequence_number;
        m_pull_expected_seq = sequence_number;
        m_initialized.store(true, std::memory_order_relaxed);
        spdlog::debug("[PUSH] INIT | base_seq:{}, next_seq:{} (new packet:{})",
            m_push_base_seq.load(), m_pull_expected_seq.load(), sequence_number);
    }

    // 检查重复包
    if (m_main_packets_buffer.contains(sequence_number)) {
        spdlog::warn("[PUSH] DUP  | seq={} (base:{})", sequence_number, m_push_base_seq.load());
        return false;
    }

    // 插入新包
    m_main_packets_buffer.emplace(sequence_number, std::move(packet_with_header));
    spdlog::debug("[PUSH] STORED | seq={} (buffer size:{})", sequence_number, m_main_packets_buffer.size());

    // 动态更新基准序列号
    if (is_sequence_older(sequence_number, m_push_base_seq.load(std::memory_order_relaxed))) {
        const uint32_t old_base = m_push_base_seq.load();
        m_push_base_seq.store(sequence_number, std::memory_order_relaxed);
        spdlog::info("[PUSH] BASE  | updated from {} to {} (new packet:{})",
            old_base, m_push_base_seq.load(), sequence_number);
    }

    // 容量管理：删除旧包时确保base_seq有效性
    while (m_main_packets_buffer.size() > MAX_ADAPTIVE_BUFFER_MAP_SIZE) {
        const auto oldest_it = m_main_packets_buffer.begin();
        const uint32_t erased_seq = oldest_it->first;
        m_main_packets_buffer.erase(oldest_it);
        spdlog::debug("[PUSH] PURGE | seq={} (buffer size:{})", erased_seq, m_main_packets_buffer.size());

        if (erased_seq == m_push_base_seq.load(std::memory_order_relaxed)) {
            if (auto new_base_it = m_main_packets_buffer.begin(); new_base_it != m_main_packets_buffer.end()) {
                m_push_base_seq.store(new_base_it->first, std::memory_order_relaxed);

                spdlog::info("[PUSH] BASE  | auto updated to {} after purge", new_base_it->first);
            } else {
                m_push_base_seq.store(0, std::memory_order_relaxed);
                spdlog::warn("[PUSH] RESET | buffer emptied, base_seq reset to 0");
            }
        }
    }

    return true;
}

size_t adaptive_buffer::pull_buffer_data(float* output_buffer, size_t need_samples_size)
{
    if (!output_buffer || need_samples_size == 0) {
        spdlog::warn("[PULL] INVALID | output buffer");
        return 0;
    }

    size_t filled_samples = 0;

    if (!m_initialized) {
        // 填充静音
        const size_t silence_samples = need_samples_size;
        std::memset(output_buffer, 0, silence_samples * sizeof(float));
        return silence_samples;
    }

    // 处理残余数据
    if (!m_last_pull_remains.empty()) {
        const size_t remains_samples = m_last_pull_remains.size() / sizeof(float);
        const size_t copy_samples = std::min(remains_samples, need_samples_size);

        std::memcpy(output_buffer, m_last_pull_remains.data(), copy_samples * sizeof(float));
        filled_samples += copy_samples;
        spdlog::debug("[PULL] REMNANT | used {} samples (remaining:{})",
            copy_samples, remains_samples - copy_samples);

        if (copy_samples < remains_samples) {
            m_last_pull_remains.erase(m_last_pull_remains.begin(),
                m_last_pull_remains.begin() + copy_samples * sizeof(float));
        } else {
            m_last_pull_remains.clear();
        }
    }

    uint32_t current_expected_seq = m_pull_expected_seq.load(std::memory_order_acquire);
    const uint32_t base_seq = m_push_base_seq.load(std::memory_order_relaxed);

    // 对齐基准序列号（处理pull滞后）
    if (is_sequence_older(current_expected_seq, base_seq)) {
        spdlog::warn("[PULL] SYNC  | jump from {} to base_seq:{}", current_expected_seq, base_seq);
        current_expected_seq = base_seq;
        m_pull_expected_seq.store(current_expected_seq, std::memory_order_release);
    }

    // 主数据填充循环
    while (filled_samples < need_samples_size) {
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

            const uint8_t* packet_data = packet.data() + sizeof(AudioPacketHeader);
            const size_t packet_bytes = packet.size() - sizeof(AudioPacketHeader);

            // 严格校验数据对齐
            if (packet_bytes % sizeof(float) != 0) {
                spdlog::warn("[PULL] Invalid packet data size {} at seq={}", packet_bytes, current_expected_seq);
                m_main_packets_buffer.erase(it);
                current_expected_seq++;
                continue;
            }

            // 正常处理有效包
            const size_t packet_samples = packet_bytes / sizeof(float);
            const size_t remaining_need = need_samples_size - filled_samples;
            const size_t copy_samples = std::min(packet_samples, remaining_need);

            std::memcpy(output_buffer + filled_samples, packet_data, copy_samples * sizeof(float));
            filled_samples += copy_samples;

            // 处理包内残余数据
            if (copy_samples < packet_samples) {
                const size_t remains_bytes = (packet_samples - copy_samples) * sizeof(float);
                m_last_pull_remains.assign(
                    packet_data + copy_samples * sizeof(float),
                    packet_data + copy_samples * sizeof(float) + remains_bytes);
                spdlog::debug("[PULL] ENOUGH SPLIT  | seq={} (copied:{}, remains:{})",
                    current_expected_seq, copy_samples, packet_samples - copy_samples);
            }

            m_main_packets_buffer.erase(it);
            /*
            spdlog::debug("[PULL] EXPIRE ERASE  | seq={} (buffer size:{})",
                current_expected_seq, m_main_packets_buffer.size());
            */
            current_expected_seq = (current_expected_seq == std::numeric_limits<uint32_t>::max()) ? 0 : current_expected_seq + 1;

        } else {
            // current_expected_seq位置的包没有
            // 动态跳跃逻辑
            auto next_it = m_main_packets_buffer.lower_bound(current_expected_seq);

            if (next_it != m_main_packets_buffer.end()) {

                const uint32_t gap = (next_it->first >= current_expected_seq) ?
                    (next_it->first - current_expected_seq) :
                    (UINT32_MAX - current_expected_seq + next_it->first + 1);
                spdlog::warn("[PULL] JUMP  | from {} to {} (gap:{})", current_expected_seq, next_it->first, gap);

                current_expected_seq = next_it->first;
                continue;
            }

            // 静音填充
            const size_t silence_samples = need_samples_size - filled_samples;
            std::memset(output_buffer + filled_samples, 0, silence_samples * sizeof(float));
            filled_samples += silence_samples;
            spdlog::warn("[PULL] GAP    | filled {} silence at seq={}", silence_samples, current_expected_seq);

            // 略微增加一下
            if (++m_muted_count % 5 == 0) {
                current_expected_seq = (current_expected_seq == std::numeric_limits<uint32_t>::max()) ? 0 : current_expected_seq + 1;
            }

            break;
        }
    }

    m_pull_expected_seq.store(current_expected_seq, std::memory_order_release);

    spdlog::debug("[PULL] FINISH | filled {}/{} (next_seq:{}) (buffer size:{})",
        filled_samples, need_samples_size, current_expected_seq, m_main_packets_buffer.size());

    return filled_samples;
}