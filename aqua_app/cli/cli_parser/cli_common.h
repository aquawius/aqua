#ifndef AQUA_CLI_PARSER_COMMON_H
#define AQUA_CLI_PARSER_COMMON_H

// CLI parser 共用：音频参数解析、F 推导、解析结果枚举。

#include "aqua/audio/audio_format.h"
#include "aqua/audio/buffer/buffer_config.h"
#include "aqua/audio/packetizer/audio_packetizer.h"
#include "aqua/logger/logger.h"
#include "aqua/net/address/address_utils.h"
#include "aqua/net/grpc/grpc_config.h"
#include "aqua/net/udp/udp_config.h"
#include "aqua/runtime/runtime_config.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>

#ifdef _WIN32
#include <windows.h>
#endif

namespace aqua::cli {

// CLI 文本输出统一使用 UTF-8。必须在参数解析前调用，因为 --help/--list-devices
// 可能在 init_logger() 之前直接写入 stdout/stderr。
inline void configure_console_utf8() noexcept
{
#ifdef _WIN32
    if (HANDLE console = ::GetStdHandle(STD_OUTPUT_HANDLE);
        console != INVALID_HANDLE_VALUE && console != nullptr) {
        DWORD mode = 0;
        if (::GetConsoleMode(console, &mode)) {
            (void)::SetConsoleOutputCP(CP_UTF8);
        }
    }
#endif
}

// 解析结果：Run = 成功（config 已填充）；Help = 已打印 usage，应退出(0)；
// ListDevices = 已列出设备，应退出(0)；Version = 已打印版本，应退出(0)；
// Error = 参数错误，应退出(1)。
enum class ParseOutcome {
    Run,
    Help,
    ListDevices,
    Version,
    Error,
};

// 解析结果 → main 的退出码。Run 返回 nullopt（继续执行），
// Help/ListDevices/Version 返回 0，Error 返回 1。供两个 CLI main 共用，避免重复 switch。
[[nodiscard]] inline std::optional<int> cli_exit_code(ParseOutcome outcome)
{
    switch (outcome) {
    case ParseOutcome::Run:
        return std::nullopt;
    case ParseOutcome::Help:
    case ParseOutcome::ListDevices:
    case ParseOutcome::Version:
        return 0;
    case ParseOutcome::Error:
        return 1;
    }
    return 1;
}

// encoding 字符串 → AudioEncoding；无法识别返回 nullopt。
inline std::optional<audio::AudioEncoding> parse_encoding(std::string_view name)
{
    if (name == "s16") {
        return audio::AudioEncoding::PCM_S16LE;
    }
    if (name == "s24") {
        return audio::AudioEncoding::PCM_S24LE;
    }
    if (name == "s32") {
        return audio::AudioEncoding::PCM_S32LE;
    }
    if (name == "f32") {
        return audio::AudioEncoding::PCM_F32LE;
    }
    if (name == "u8") {
        return audio::AudioEncoding::PCM_U8;
    }
    return std::nullopt;
}

inline audio::AudioFormat make_format(audio::AudioEncoding enc, std::uint32_t channels,
    std::uint32_t sample_rate)
{
    audio::AudioFormat fmt;
    fmt.encoding = enc;
    fmt.channels = channels;
    fmt.sample_rate = sample_rate;
    return fmt;
}

// MTU 净荷预算：IPv6-safe 1500 − 40 − 8 − 12 = 1440，再让 40B 隧道/封装余量 = 1400
// （取值理由见 udp_config.h UDP_AUDIO_PAYLOAD_BYTES）。
inline constexpr std::size_t kMtuPayloadBudget = config::UDP_AUDIO_PAYLOAD_BYTES;
inline constexpr std::uint16_t kDefaultRpcPort = aqua::config::DEFAULT_RPC_PORT;
inline constexpr std::uint16_t kDefaultUdpPort = aqua::config::DEFAULT_UDP_PORT;

inline std::string_view audio_encoding_name(audio::AudioEncoding encoding) noexcept
{
    switch (encoding) {
    case audio::AudioEncoding::PCM_S16LE:
        return "s16";
    case audio::AudioEncoding::PCM_S32LE:
        return "s32";
    case audio::AudioEncoding::PCM_F32LE:
        return "f32";
    case audio::AudioEncoding::PCM_S24LE:
        return "s24";
    case audio::AudioEncoding::PCM_U8:
        return "u8";
    case audio::AudioEncoding::INVALID:
        return "invalid";
    }
    return "invalid";
}
inline constexpr std::uint32_t kMinPacketFrames = aqua::config::MIN_FRAMES_PER_SLOT;
inline constexpr std::uint32_t kMaxJbCapacitySlots = aqua::config::JB_MAX_CAPACITY_SLOTS;
inline constexpr std::uint32_t kMaxAudioQueueCapacitySlots = aqua::config::MAX_AUDIO_QUEUE_CAPACITY_SLOTS;
// 交接队列容量下限：必须大于 pacing 追赶深度，否则队列先于追赶触发而丢最新帧。
inline constexpr std::uint32_t kMinAudioQueueCapacitySlots
    = aqua::config::MIN_AUDIO_QUEUE_CAPACITY_SLOTS;

inline bool validate_ip_literal(const std::string& value, const char* option_name, bool allow_wildcard = true)
{
    if (value.empty()) {
        std::cerr << "invalid " << option_name << ": value must not be empty\n";
        return false;
    }
    try {
        const auto address = ::aqua::net::parse_ip_address(value);
        if (!allow_wildcard && address.is_unspecified()) {
            std::cerr << "invalid " << option_name << ": address must be a concrete reachable IP\n";
            return false;
        }
    } catch (const std::exception& e) {
        std::cerr << "invalid " << option_name << ": " << format_exception_message(e) << "\n";
        return false;
    }
    return true;
}

// F 确定：显式指定则用指定值（并校验 ≤ MTU 预算）；否则按 MTU 预算与包时长
// 上限（UDP_AUDIO_MAX_PACKET_MS）取小反推。
// 返回 0 表示非法（显式 F 超 MTU 预算 / 溢出，或自动推导失败）。
inline std::uint32_t resolve_frame_count(std::uint32_t explicit_packet_frames,
    const audio::AudioFormat& fmt)
{
    std::uint32_t frames = 0;
    if (explicit_packet_frames != 0) {
        // 显式 F 换算成字节数（bytes_for_frames 溢出返回 0），必须 ≤ MTU 预算，
        // 否则一个 AudioFrame 会超过单个 UDP 包容量导致 IP 分片（实时音频不可接受）。
        if (explicit_packet_frames < kMinPacketFrames) {
            return 0;
        }
        const auto bytes = fmt.bytes_for_frames(explicit_packet_frames);
        if (bytes == 0 || bytes > kMtuPayloadBudget) {
            return 0;
        }
        frames = explicit_packet_frames;
    } else {
        // auto-F：MTU 预算与包时长上限取小（与 ServerRuntime 的 auto-F 同口径）。
        const auto budget_frames = audio::frame_count_for_budget(fmt, kMtuPayloadBudget);
        const auto duration_frames = audio::frame_count_for_duration(
            fmt, config::UDP_AUDIO_MAX_PACKET_MS);
        if (budget_frames == 0 || duration_frames == 0) {
            return 0;
        }
        frames = std::min(budget_frames, duration_frames);
    }
    // 包时长下界（UDP_AUDIO_MIN_PACKET_MS）：极端格式（多声道+高位深+高采样率）
    // 下 MTU 预算只能给出极小的 F，包率高到 pacing 无法实现、交接队列必然持续
    // 溢出。这类格式在 1400B payload 预算下无法可靠传输，与 ServerRuntime 同口径
    // 拒绝（否则启动成功却疯狂丢帧）。
    if (fmt.sample_rate == 0) {
        return 0;
    }
    const double packet_ms
        = static_cast<double>(frames) * 1000.0 / static_cast<double>(fmt.sample_rate);
    if (packet_ms < config::UDP_AUDIO_MIN_PACKET_MS) {
        return 0;
    }
    return frames;
}

} // namespace aqua::cli

#endif // AQUA_CLI_PARSER_COMMON_H
