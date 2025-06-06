//
// Created by aquawius on 25-1-8.
//

#ifndef CMDLINE_PARSER_H
#define CMDLINE_PARSER_H

#include "audio_format_common.hpp"

#include <cxxopts.hpp>
#include <spdlog/spdlog.h>
#include <string>

namespace aqua
{
    class cmdline_parser
    {
    public:
        struct parse_result
        {
            bool help = false;
            bool version = false;
            spdlog::level::level_enum log_level = spdlog::level::info;
            std::string bind_address;
            uint16_t port = 10120;
            audio_common::AudioEncoding encoding = audio_common::AudioEncoding::INVALID;
            uint32_t channels = 0;
            uint32_t sample_rate = 0;
        };

        cmdline_parser(int argc, const char* argv[]);
        parse_result parse();
        static std::string get_help_string();

    private:
        cxxopts::Options m_options;
        int m_argc;
        const char** m_argv;
    };
}

#endif // CMDLINE_PARSER_H
