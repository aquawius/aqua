#ifndef AQUA_CLI_PARSER_CLIENT_H
#define AQUA_CLI_PARSER_CLIENT_H

#include "aqua/runtime/client_runtime.h"
#include "cli_common.h"

namespace aqua::cli {

// 解析 client CLI 参数并校验；成功填 config 并返回 Run，--help 返回 Help，参数错误返回 Error。
ParseOutcome parse_client_cli(int argc, char** argv, runtime::ClientRuntimeConfig& config, LogLevel& log_level);

} // namespace aqua::cli

#endif // AQUA_CLI_PARSER_CLIENT_H
