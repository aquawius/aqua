//
// Created by aquawius on 25-2-6.
//

#ifndef CMDLINE_PARSER_CLIENT_H
#define CMDLINE_PARSER_CLIENT_H

#include <cxxopts.hpp>
#include <network_client.h>
#include <spdlog/spdlog.h>
#include <string>

namespace aqua_client {

class cmdline_parser {
public:
    struct parse_result {
        bool help = false;
        bool version = false;
        spdlog::level::level_enum log_level = spdlog::level::info;
        std::string server_address;
        uint16_t server_rpc_port = 10120;
        std::string client_address;
        uint16_t client_udp_port = 0; // 0 表示需要随机生成
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
#endif // CMDLINE_PARSER_CLIENT_H