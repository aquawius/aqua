#include "aqua.h"

#include "core/public/audio_format.h"

#include <gtest/gtest.h>

#include <cstring>

// C API 只测不依赖网络/音频硬件的部分：版本、日志级别、句柄生命周期、
// 配置默认值、状态码、空参数安全、编码数值与内部 AudioEncoding 同步。
// 实际连接/播放走集成或实机验证（WASAPI/gRPC 不在单测范围，见 AGENT.md §29.2）。

TEST(CapiTest, VersionNonEmpty)
{
    const char* v = aqua_version();
    ASSERT_NE(v, nullptr);
    EXPECT_GT(std::strlen(v), 0u);
}

TEST(CapiTest, SetLogLevelAllValidLevels)
{
    EXPECT_EQ(aqua_set_log_level(AQUA_LOG_TRACE), AQUA_OK);
    EXPECT_EQ(aqua_set_log_level(AQUA_LOG_DEBUG), AQUA_OK);
    EXPECT_EQ(aqua_set_log_level(AQUA_LOG_INFO), AQUA_OK);
    EXPECT_EQ(aqua_set_log_level(AQUA_LOG_WARN), AQUA_OK);
    EXPECT_EQ(aqua_set_log_level(AQUA_LOG_ERROR), AQUA_OK);
}

TEST(CapiTest, SetLogLevelRejectsInvalid)
{
    EXPECT_EQ(aqua_set_log_level(static_cast<aqua_log_level_t>(999)), AQUA_ERR_INVALID_ARGUMENT);
}

TEST(CapiTest, EncodingValuesMatchInternalAudioEncoding)
{
    EXPECT_EQ(static_cast<int>(aqua::AudioEncoding::Invalid), AQUA_ENCODING_INVALID);
    EXPECT_EQ(static_cast<int>(aqua::AudioEncoding::PcmS16LE), AQUA_ENCODING_PCM_S16LE);
    EXPECT_EQ(static_cast<int>(aqua::AudioEncoding::PcmS32LE), AQUA_ENCODING_PCM_S32LE);
    EXPECT_EQ(static_cast<int>(aqua::AudioEncoding::PcmF32LE), AQUA_ENCODING_PCM_F32LE);
    EXPECT_EQ(static_cast<int>(aqua::AudioEncoding::PcmS24LE), AQUA_ENCODING_PCM_S24LE);
    EXPECT_EQ(static_cast<int>(aqua::AudioEncoding::PcmU8), AQUA_ENCODING_PCM_U8);
}

TEST(CapiTest, ClientCreateDestroyAndInitialState)
{
    aqua_client_t* c = aqua_client_create();
    ASSERT_NE(c, nullptr);
    EXPECT_EQ(aqua_client_state(c), AQUA_CLIENT_IDLE);
    EXPECT_STREQ(aqua_client_last_error(c), "");
    aqua_client_destroy(c);
}

TEST(CapiTest, ServerCreateDestroy)
{
    aqua_server_t* s = aqua_server_create();
    ASSERT_NE(s, nullptr);
    EXPECT_STREQ(aqua_server_last_error(s), "");
    aqua_server_destroy(s);
}

TEST(CapiTest, IsRunningFalseBeforeStart)
{
    aqua_client_t* c = aqua_client_create();
    ASSERT_NE(c, nullptr);
    EXPECT_EQ(aqua_client_is_running(c), 0);
    aqua_client_destroy(c);

    aqua_server_t* s = aqua_server_create();
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(aqua_server_is_running(s), 0);
    aqua_server_destroy(s);
}

TEST(CapiTest, IsRunningNullHandleSafe)
{
    EXPECT_EQ(aqua_client_is_running(nullptr), 0);
    EXPECT_EQ(aqua_server_is_running(nullptr), 0);
}

TEST(CapiTest, ClientConfigInitDefaults)
{
    aqua_client_config_t cfg;
    aqua_client_config_init(&cfg);
    EXPECT_STREQ(cfg.server_ip, "127.0.0.1");
    EXPECT_EQ(cfg.server_rpc_port, 50051);
    EXPECT_EQ(cfg.jitter_buffer_ms, 0u);
    EXPECT_EQ(cfg.playback_ringbuffer_size, 0u);
    EXPECT_EQ(cfg.auto_reconnect, 0);
    EXPECT_STREQ(cfg.client_name, "aqua_client");
}

TEST(CapiTest, ServerConfigInitDefaults)
{
    aqua_server_config_t cfg;
    aqua_server_config_init(&cfg);
    EXPECT_STREQ(cfg.bind_ip, "0.0.0.0");
    EXPECT_EQ(cfg.rpc_port, 50051);
    EXPECT_EQ(cfg.udp_port, 50000);
    EXPECT_EQ(cfg.capture_ringbuffer_size, 0u);
}

TEST(CapiTest, NullArgumentsAreSafe)
{
    // 所有入口对 NULL 句柄/参数要么安全无操作，要么返回负码，不崩溃。
    aqua_client_destroy(nullptr);
    aqua_server_destroy(nullptr);
    aqua_client_config_init(nullptr);
    aqua_server_config_init(nullptr);

    EXPECT_EQ(aqua_client_state(nullptr), AQUA_CLIENT_IDLE);
    EXPECT_STREQ(aqua_client_last_error(nullptr), "");
    EXPECT_STREQ(aqua_server_last_error(nullptr), "");

    EXPECT_EQ(aqua_client_shutdown(nullptr), AQUA_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(aqua_server_shutdown(nullptr), AQUA_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(aqua_client_start(nullptr, nullptr, nullptr), AQUA_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(aqua_server_start(nullptr, nullptr, nullptr), AQUA_ERR_INVALID_ARGUMENT);
}

TEST(CapiTest, StartRejectsNullConfig)
{
    aqua_client_t* c = aqua_client_create();
    ASSERT_NE(c, nullptr);
    EXPECT_EQ(aqua_client_start(c, nullptr, nullptr), AQUA_ERR_INVALID_ARGUMENT);
    aqua_client_destroy(c);

    aqua_server_t* s = aqua_server_create();
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(aqua_server_start(s, nullptr, nullptr), AQUA_ERR_INVALID_ARGUMENT);
    aqua_server_destroy(s);
}

TEST(CapiTest, GetDiagnosticsRejectsNullArgs)
{
    aqua_client_t* c = aqua_client_create();
    ASSERT_NE(c, nullptr);

    // out 为 NULL → 非法参数
    EXPECT_EQ(aqua_client_get_diagnostics(c, nullptr), AQUA_ERR_INVALID_ARGUMENT);
    // 句柄为 NULL → 非法参数
    EXPECT_EQ(aqua_client_get_diagnostics(nullptr, nullptr), AQUA_ERR_INVALID_ARGUMENT);

    aqua_client_destroy(c);
}

TEST(CapiTest, GetDiagnosticsNotAvailableBeforeStart)
{
    aqua_client_t* c = aqua_client_create();
    ASSERT_NE(c, nullptr);

    // 未 start → 尚无诊断快照（进入播放态后每 3s 才刷新）。
    aqua_diagnostics_t d { };
    EXPECT_EQ(aqua_client_get_diagnostics(c, &d), AQUA_ERR_NOT_AVAILABLE);

    aqua_client_destroy(c);
}
