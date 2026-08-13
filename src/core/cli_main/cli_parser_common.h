#ifndef AQUA_CLI_PARSER_COMMON_H
#define AQUA_CLI_PARSER_COMMON_H

#include <cstdint>
#include <optional>
#include <string>

namespace aqua {

// 解析端口号字符串，校验范围 1..65535。
// 失败时填充 error 并返回 std::nullopt。
// cli_parser_client.cpp 与 cli_parser_server.cpp 共用。
inline std::optional<uint16_t> parse_port(const std::string& value, const std::string& name,
                                          std::string& error) {
    try {
        int port = std::stoi(value);
        if (port <= 0 || port > 65535) {
            error = name + " must be in range 1..65535";
            return std::nullopt;
        }
        return static_cast<uint16_t>(port);
    } catch (const std::exception&) {
        error = name + " must be a valid port number";
        return std::nullopt;
    }
}

} // namespace aqua

#endif // AQUA_CLI_PARSER_COMMON_H
