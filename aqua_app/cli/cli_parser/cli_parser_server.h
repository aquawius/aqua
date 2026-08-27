#ifndef AQUA_CLI_PARSER_SERVER_H
#define AQUA_CLI_PARSER_SERVER_H

#include "aqua/runtime/server_runtime.h"
#include "cli_common.h"

namespace aqua::cli {

// 解析 server CLI 参数并校验；成功填 config 并返回 Run，--help 返回 Help，参数错误返回 Error。
ParseOutcome parse_server_cli(int argc, char** argv, runtime::ServerRuntimeConfig& config);

} // namespace aqua::cli

#endif // AQUA_CLI_PARSER_SERVER_H
