//
// Created by aquawius on 25-2-2.
//

#include "adaptive_buffer.h"

#include <boost/endian/conversion.hpp>
#include <cstring>
#include <spdlog/spdlog.h>

adaptive_buffer::adaptive_buffer()
    : m_total_samples_in_buffer(0)
{
}

bool adaptive_buffer::put_buffer_packets(std::vector<uint8_t>&& packet_with_header)
{
    if (packet_with_header.size() < sizeof(AudioPacketHeader)) {
        spdlog::warn("Received invalid packet size: {} (min required: {})",
            packet_with_header.size(), sizeof(AudioPacketHeader));
        return false;
    }

    // 提取包头信息
    const auto* header = reinterpret_cast<const AudioPacketHeader*>(packet_with_header.data());
    uint32_t sequence_number = boost::endian::big_to_native(header->sequence_number);
    // uint64_t timestamp = boost::endian::big_to_native(header->timestamp);

    // 计算样本数
    const size_t packet_payload_size = packet_with_header.size() - sizeof(AudioPacketHeader);
    const size_t packet_samples = packet_payload_size / sizeof(float);

    m_packet_queue.push_back(std::move(packet_with_header));
    m_total_samples_in_buffer += packet_samples;

    // 加入后状态记录
    if (spdlog::get_level() <= spdlog::level::trace) {
        spdlog::trace("[adaptive_buffer] [I] NEW [Seq #{} Samp: {}],\tBuf: {} Queue: {}",
            sequence_number, packet_samples, m_total_samples_in_buffer, m_packet_queue.size());
    }
    // 缓冲区溢出处理
    while (m_total_samples_in_buffer > MAX_BUFFERED_SAMPLES && !m_packet_queue.empty()) {
        const auto& old_packet = m_packet_queue.front();

        // 提取被丢弃数据包的信息
        const auto* old_header = reinterpret_cast<const AudioPacketHeader*>(old_packet.data());
        uint32_t old_seq = boost::endian::big_to_native(old_header->sequence_number);
        // uint64_t old_ts = boost::endian::big_to_native(old_header->timestamp);

        const size_t old_payload = old_packet.size() - sizeof(AudioPacketHeader);
        const size_t old_samples = old_payload / sizeof(float);

        // 更新状态
        m_total_samples_in_buffer -= old_samples;
        m_packet_queue.pop_front();

        spdlog::warn("[adaptive_buffer] [I] Buffer FULL! DROP [Seq #{} Samp: {}],\tBuf: {} Queue: {}",
            old_seq, old_samples, m_total_samples_in_buffer, m_packet_queue.size());
    }

    return true;
}

size_t adaptive_buffer::get_samples(float* output_buffer, const size_t need_samples)
{
    if (!output_buffer || need_samples == 0) {
        spdlog::warn("[adaptive_buffer] [O] Invalid output request");
        return 0;
    }

    size_t filled_samples = 0;

    // 处理上次残余数据
    if (!last_get_remains.empty()) {
        size_t remains_samples = last_get_remains.size() / sizeof(float);
        size_t need_copy_samples = std::min(remains_samples, need_samples);

        const float* remains_data = reinterpret_cast<const float*>(last_get_remains.data());
        std::memcpy(output_buffer, remains_data, need_copy_samples * sizeof(float));
        filled_samples += need_copy_samples;

        // 别忘了
        m_total_samples_in_buffer -= need_copy_samples;

        if (need_copy_samples < remains_samples) {
            // 应该极少出现下面这样的情况, 甚至不可能到这
            // 如果 need_copy_samples 小于 remains_samples，说明处理完需要的数据仍然还有剩余的数据未被处理。
            // 此时，计算新的残余数据的大小，并创建一个新的 std::vector<uint8_t> 来存储这些剩余数据。
            spdlog::warn("[adaptive_buffer] [O] Still remaining data that has not been processed, May get/put not sync.");
            std::vector<uint8_t> new_remains(
                last_get_remains.begin() + need_copy_samples * sizeof(float),
                last_get_remains.end());
            // 将数据放到残余缓冲
            last_get_remains.swap(new_remains);
        } else {
            // 这里应该是绝大部分的情况
            last_get_remains.clear();
        }
        if (spdlog::get_level() <= spdlog::level::trace) {
            spdlog::trace("[adaptive_buffer] [O] PROC Reuse [remains/target:{}/{} filled/need:{}/{}] Residual: {} \tBuf: {} Queue: {}",
                remains_samples,
                need_copy_samples,
                filled_samples, need_samples,
                last_get_remains.size() / sizeof(float),
                m_total_samples_in_buffer,
                m_packet_queue.size());
        }
    }

    // 从数据包队列提取数据
    while (filled_samples < need_samples) {
        if (m_packet_queue.empty()) {
            size_t silence_samples = need_samples - filled_samples;
            std::memset(output_buffer + filled_samples, 0, silence_samples * sizeof(float));
            filled_samples += silence_samples;

            spdlog::warn("[adaptive_buffer] [O] NOT enough for need_samples, Filled {} silence samples. filled/need:{}/{}\tBuf: {} Queue: {}",
                silence_samples, filled_samples, need_samples, m_total_samples_in_buffer, m_packet_queue.size());
            break;
        }

        auto& packet = m_packet_queue.front();

        // 提取包头信息
        uint32_t sequence_number = boost::endian::big_to_native(
            *reinterpret_cast<const uint32_t*>(packet.data()));
        uint64_t timestamp = boost::endian::big_to_native(
            *reinterpret_cast<const uint64_t*>(packet.data() + sizeof(uint32_t)));

        const uint8_t* packet_data = packet.data() + sizeof(AudioPacketHeader);
        size_t packet_payload_size = packet.size() - sizeof(AudioPacketHeader);
        size_t packet_samples = packet_payload_size / sizeof(float);
        const float* audio_data = reinterpret_cast<const float*>(packet_data);

        size_t samples_needed = need_samples - filled_samples;

        if (packet_samples <= samples_needed) {
            // 使用整个数据包
            std::memcpy(output_buffer + filled_samples, audio_data, packet_samples * sizeof(float));
            filled_samples += packet_samples;
            m_total_samples_in_buffer -= packet_samples;
            m_packet_queue.pop_front();
            if (spdlog::get_level() <= spdlog::level::trace) {
                spdlog::trace("[adaptive_buffer] [O] PROC Full [Seq #{} Samp {}] Needed: {}\tBuf: {} Queue: {}",
                    sequence_number, packet_samples, samples_needed, m_total_samples_in_buffer, m_packet_queue.size());
            }
        } else {
            // 使用部分数据包
            std::memcpy(output_buffer + filled_samples, audio_data, samples_needed * sizeof(float));
            filled_samples += samples_needed;

            // 保存剩余部分
            size_t remaining_samples = packet_samples - samples_needed;
            last_get_remains.resize(remaining_samples * sizeof(float));
            std::memcpy(last_get_remains.data(), audio_data + samples_needed,
                remaining_samples * sizeof(float));

            m_total_samples_in_buffer -= samples_needed;
            m_packet_queue.pop_front();
            if (spdlog::get_level() <= spdlog::level::trace) {
                spdlog::trace("[adaptive_buffer] [O] PROC Part [Seq #{} Samp {}] [Used: {} Saved: {}]\tBuf: {} Queue: {}",
                    sequence_number, packet_samples, samples_needed, remaining_samples, m_total_samples_in_buffer, m_packet_queue.size());
            }
            break;
        }
    }

    return filled_samples;
}