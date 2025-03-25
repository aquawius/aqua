//
// Created by aquawius on 25-1-8.
//

#include <iostream>
#include <cxxopts.hpp>
#include "version.h"
#include "cmdline_parser.h"
#include "network_server.h"
#include <spdlog/fmt/fmt.h>

aqua::cmdline_parser::cmdline_parser(int argc, const char* argv[])
    : m_options(aqua_server_BINARY_NAME, get_help_string())
      , m_argc(argc)
      , m_argv(argv)
{
    // clang-format off
    m_options.add_options()
        ("h,help", "Print usage")
        ("b,bind", "Server bind address (IP). Default uses auto-detected private address", cxxopts::value<std::string>())
        ("p,port", "Server port (default: 10120)", cxxopts::value<uint16_t>()->default_value("10120"))
        ("v,version", "Show version")
        ("V,verbose", "Set log level (Not set=info, V=debug, VV=trace)")
        ("e,encoding", "Audio encoding format (s16le, s32le, f32le, s24le, u8), default: f32le", cxxopts::value<std::string>())
        ("c,channels", "Number of audio channels (1-8)", cxxopts::value<uint32_t>())
        ("r,rate", "Sample rate in Hz (8000-384000)", cxxopts::value<uint32_t>()) ;
    // clang-format on
}

aqua::cmdline_parser::parse_result aqua::cmdline_parser::parse()
{
    try {
        parse_result result;
        auto parsed = m_options.parse(m_argc, m_argv);

        result.help = parsed.count("help") > 0;
        result.version = parsed.count("version") > 0;

        // 处理日志级别
        int verbose_count = parsed.count("verbose");
        switch (verbose_count) {
        case 0:
            result.log_level = spdlog::level::info;
            break;
        case 1:
            result.log_level = spdlog::level::debug;
            break;
        default:
            result.log_level = spdlog::level::trace;
            break;
        }

        // 处理绑定地址
        if (parsed.count("bind")) {
            result.bind_address = parsed["bind"].as<std::string>();
        }

        // 处理端口
        if (parsed.count("port")) {
            result.port = parsed["port"].as<uint16_t>();
        }

        // 处理音频格式
        if (parsed.count("encoding")) {
            std::string enc = parsed["encoding"].as<std::string>();
            if (enc == "s16le")
                result.encoding = audio_common::AudioEncoding::PCM_S16LE;
            else if (enc == "s32le")
                result.encoding = audio_common::AudioEncoding::PCM_S32LE;
            else if (enc == "f32le")
                result.encoding = audio_common::AudioEncoding::PCM_F32LE;
            else if (enc == "s24le")
                result.encoding = audio_common::AudioEncoding::PCM_S24LE;
            else if (enc == "u8")
                result.encoding = audio_common::AudioEncoding::PCM_U8;
            else {
                result.encoding = audio_common::AudioEncoding::INVALID;
                throw std::runtime_error("Invalid audio encoding format");
            }
        }

        if (parsed.count("channels")) {
            result.channels = parsed["channels"].as<uint32_t>();
            if (result.channels < 1 || result.channels > 8) {
                throw std::runtime_error("Invalid number of channels (must be 1-8)");
            }
        }

        if (parsed.count("rate")) {
            result.sample_rate = parsed["rate"].as<uint32_t>();
            if (result.sample_rate < 8000 || result.sample_rate > 384000) {
                throw std::runtime_error("Invalid sample rate (must be 8000-384000 Hz)");
            }
        }

        return result;
    } catch (const std::exception& e) {
        throw std::runtime_error(std::string("Failed to parse command line: ") + e.what());
    }
}

std::string aqua::cmdline_parser::get_help_string()
{
    std::string default_address = network_server::get_default_address();

    std::string help_string;

    // 程序名称和描述
    help_string += fmt::format("{} - Audio Streaming Server\n", aqua_server_BINARY_NAME);
    help_string += "Stream audio from your device to network clients\n\n";

    // 基本用法
    help_string += "USAGE:\n";
    help_string += fmt::format("  {} [OPTIONS]\n\n", aqua_server_BINARY_NAME);

    // 选项分组 - 网络设置
    help_string += "NETWORK OPTIONS:\n";
    help_string += "  -b, --bind <address>    Server bind address (IP)\n";
    help_string += fmt::format("                        Default: {}\n", default_address.empty() ? "0.0.0.0" : default_address);
    help_string += "  -p, --port <port>       Server port number\n";
    help_string += "                        Default: 10120\n\n";

    // 选项分组 - 音频设置
    help_string += "AUDIO OPTIONS:\n";
    help_string += "  -e, --encoding <format> Audio encoding format\n";
    help_string += "                        Supported: s16le, s32le, f32le, s24le, u8\n";
    help_string += "  -c, --channels <num>    Number of audio channels (1-8)\n";
    help_string += "  -r, --rate <hz>         Sample rate in Hz (8000-384000)\n";
    help_string += "If not provide audio format, will use system default audio format.\n\n";

    // 选项分组 - 其他选项
    help_string += "OTHER OPTIONS:\n";
    help_string += "  -V, --verbose           Increase logging verbosity\n";
    help_string += "                        Not set = info, -V = debug, -VV = trace\n";
    help_string += "  -h, --help              Display this help message\n";
    help_string += "  -v, --version           Display version information\n\n";

    // 使用示例
    help_string += "EXAMPLES:\n";
    help_string += fmt::format("  # Start server with default settings\n  {}\n\n", aqua_server_BINARY_NAME);
    help_string += fmt::format("  # Start server on specific address and port, or bind 0.0.0.0 to serve all interfaces\n"
        "  {} -b 192.168.1.100 -p 8080\n"
        "  {} -b 0.0.0.0\n\n", aqua_server_BINARY_NAME, aqua_server_BINARY_NAME);
    help_string += fmt::format("  # Stream 16-bit stereo audio at 48kHz\n  {} -e s16le -c 2 -r 48000\n\n",
        aqua_server_BINARY_NAME);
    help_string += fmt::format("  # Enable debug logging\n  {} -V\n\n", aqua_server_BINARY_NAME);

    // 注意事项
    help_string += "NOTES:\n";
    help_string += "  - When specifying audio format, all three parameters (encoding, channels, rate)\n";
    help_string += "    must be provided together\n";
    help_string += "  - The server will use system default audio format if no audio options are specified\n";
    help_string += "  - The server will ALWAYS use system default device output. (May auto change format)\n";

    return help_string;
}