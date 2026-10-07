// CLI 前端与 Core 的端点契约。
//
// 由来：客户端的端点规则一度有**三份**实现 —— 服务端的 parser 用 cli_common.h 的
// validate_ip_literal、客户端 parser 手抄了一份、Core 有 validate_client_endpoint。
// 客户端那份漏了 v4-mapped，于是
//
//     aqua_client_cli --rpc-ip ::ffff:127.0.0.1
//
// 能通过 CLI 层的全部检查，直到 ClientRuntime::start() 才被拒。现在 CLI 直接调用
// Core 的校验函数（cli_parser_client.cpp），本文件把这条契约钉住：
//
//   1. 非法端点必须在**解析期**（ParseOutcome::Error）被拒，而不是留到运行期；
//   2. CLI 的判决必须与 Core 一致（CLI 拒绝 ⟺ validate_client_endpoint 拒绝）。
//
// 用例只走参数解析：不碰音频设备、不绑 socket，因此可以在任何机器上跑。

#include "cli_parser_client.h"

#include "aqua/logger/logger.h"
#include "aqua/runtime/client_runtime.h"

#include <gtest/gtest.h>

#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

struct CliOutcome {
    aqua::cli::ParseOutcome outcome = aqua::cli::ParseOutcome::Error;
    std::string stderr_text;
    aqua::runtime::ClientRuntimeConfig config;
};

// 跑一次客户端参数解析；解析器用 std::cerr 报错，这里把 stderr 捕获下来供断言。
CliOutcome run_client_cli(const std::vector<std::string>& args)
{
    std::vector<std::string> owned;
    owned.emplace_back("aqua_client");
    owned.insert(owned.end(), args.begin(), args.end());

    std::vector<char*> argv;
    argv.reserve(owned.size());
    for (auto& argument : owned) {
        argv.push_back(argument.data());
    }

    CliOutcome out;
    std::ostringstream captured;
    auto* const previous = std::cerr.rdbuf(captured.rdbuf());
    aqua::LogLevel level = aqua::default_log_level();
    std::string log_file;
    out.outcome = aqua::cli::parse_client_cli(
        static_cast<int>(argv.size()), argv.data(), out.config, level, log_file);
    std::cerr.rdbuf(previous);

    out.stderr_text = captured.str();
    return out;
}

// 非法输入必须：解析期报错、消息里指明是哪条规则，且与 Core 的判决一致。
void expect_rejected(const std::vector<std::string>& args, const char* expected_substring)
{
    const auto result = run_client_cli(args);
    EXPECT_EQ(result.outcome, aqua::cli::ParseOutcome::Error) << "stderr: " << result.stderr_text;
    EXPECT_NE(result.stderr_text.find(expected_substring), std::string::npos)
        << "stderr: " << result.stderr_text;
    EXPECT_TRUE(aqua::runtime::validate_client_endpoint(result.config).has_value())
        << "CLI 拒绝了但 Core 接受，说明两侧规则已经不一致";
}

} // namespace

// 曾经的漏洞形状：CLI 层放过 v4-mapped，直到 ClientRuntime::start() 才报
// "is a v4-mapped IPv6 literal"。现在必须在解析期就拒。
TEST(CliEndpointContractTest, RejectsV4MappedRpcIp)
{
    expect_rejected({ "--rpc-ip", "::ffff:127.0.0.1" }, "v4-mapped");
}

// 通配地址同一参数喂两个用途（控制面目标 + sentinel 回落时的数据面拨号目标），
// 因此必须拒绝 —— 这是 INV-8。
TEST(CliEndpointContractTest, RejectsWildcardRpcIp)
{
    expect_rejected({ "--rpc-ip", "0.0.0.0" }, "concrete reachable IP");
    expect_rejected({ "--rpc-ip", "::" }, "concrete reachable IP");
}

TEST(CliEndpointContractTest, RejectsZeroRpcPort)
{
    expect_rejected({ "--rpc-ip", "127.0.0.1", "--rpc-port", "0" }, "rpc_port must be > 0");
}

// DNS 名与空值都不是 IP 字面量：CLI 不做名字解析。
TEST(CliEndpointContractTest, RejectsNonLiteralRpcIp)
{
    expect_rejected({ "--rpc-ip", "example.com" }, "not a valid IP literal");
    expect_rejected({ "--rpc-ip", "" }, "not a valid IP literal");
}

// link-local 缺 zone 连控制面都拨不出去（内核对无 scope 的 link-local 一律 EINVAL），
// 消息必须给出可操作的写法。
TEST(CliEndpointContractTest, RejectsZoneLessLinkLocalRpcIp)
{
    expect_rejected({ "--rpc-ip", "fe80::1" }, "zone");
}

// 合法端点必须通过，并且真的落进 config（否则后面的运行期校验会再拒一次）。
// zone 的写法是平台相关的：asio 的 Windows 解析器只认**十进制 ifIndex**，
// 非 Windows 平台把 '%' 后的内容当**接口名**（"24" 不是接口名 ⇒ 解析失败）。
// 所以带 zone 的字面量只在 Windows 上断言；非 Windows 的接口名形式由
// IpFamilyTest.LinkLocalScopeIsBorrowedFromTheReferenceAddress 覆盖，不在这里重复。
TEST(CliEndpointContractTest, AcceptsConcreteLiteralsAndFillsConfig)
{
    std::vector<const char*> concrete { "127.0.0.1", "::1" };
#ifdef _WIN32
    concrete.push_back("fe80::1%24");
#endif
    for (const char* ip : concrete) {
        const auto result = run_client_cli({ "--rpc-ip", ip });
        EXPECT_EQ(result.outcome, aqua::cli::ParseOutcome::Run) << ip << " stderr: " << result.stderr_text;
        EXPECT_EQ(result.config.rpc_ip, ip);
        EXPECT_FALSE(aqua::runtime::validate_client_endpoint(result.config).has_value()) << ip;
    }
}

// --rpc-ip 是必填项；缺失时不该落到"空串"那条规则上（消息不同，指向也不同）。
TEST(CliEndpointContractTest, MissingRpcIpIsReportedAsMissing)
{
    const auto result = run_client_cli({});
    EXPECT_EQ(result.outcome, aqua::cli::ParseOutcome::Error);
    EXPECT_NE(result.stderr_text.find("missing required option --rpc-ip"), std::string::npos)
        << "stderr: " << result.stderr_text;
}

// 回放设备 I/O 粒度：未指定时必须为 0 —— 语义是"后端取**设备允许的最小周期**"，
// 而不是引擎默认周期。这一条钉住的是低延迟的默认行为，不是格式细节：几何地板
// = ceil(本值 / F) + 1 是自适应 target 的硬下限（ADR-3），所以本值的默认值直接
// 决定 JB 能压到多低。
TEST(CliPlaybackBufferContractTest, DefaultsToDeviceMinimumPeriod)
{
    const auto result = run_client_cli({ "--rpc-ip", "127.0.0.1" });
    EXPECT_EQ(result.outcome, aqua::cli::ParseOutcome::Run) << "stderr: " << result.stderr_text;
    EXPECT_EQ(result.config.playback.frames_per_buffer, 0u)
        << "未指定 --playback-buffer-frames 时必须为 0（= 后端取设备最小周期）";
}

// 显式值必须原样落进 config：CLI 不做折算，折算属于后端契约
// （WASAPI 取最接近的 fundamental 倍数并钳到 [min, max]）——只有后端知道真实
// 设备边界。0 是显式合法值，与"未指定"同义，不做区分。
TEST(CliPlaybackBufferContractTest, ExplicitValueReachesConfigUnchanged)
{
    for (const auto frames : { 0u, 1u, 128u, 480u, 4096u }) {
        const auto result = run_client_cli(
            { "--rpc-ip", "127.0.0.1", "--playback-buffer-frames", std::to_string(frames) });
        EXPECT_EQ(result.outcome, aqua::cli::ParseOutcome::Run)
            << frames << " stderr: " << result.stderr_text;
        EXPECT_EQ(result.config.playback.frames_per_buffer, frames);
    }
}
