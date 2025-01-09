//
// Created by aquawius on 25-1-8.
//

#ifndef CMDLINE_PARSER_H
#define CMDLINE_PARSER_H

#include <string>

namespace aqua {

class cmdline_parser {
public:
    struct parse_result {
        bool help = false;
        bool version = false;
        bool list_endpoint = false;
        bool list_encoding = false;
        bool verbose = false;
        std::string bind_address;
        std::string endpoint;
        std::string encoding;
        int channels = 0;
        int sample_rate = 0;
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
