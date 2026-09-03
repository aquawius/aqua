// C API host 冒烟测试：不依赖真 server / 真音频设备。
// 覆盖：版本/枚举名查询、参数校验、生命周期状态机（Created → START_FAILED →
// Stopped）、诊断快照与连接结果的契约。

#include "aqua/c_api/aqua_capi.h"

#include <gtest/gtest.h>

namespace {

aqua_client_config_t make_config(const char* server_ip, uint16_t rpc_port)
{
    aqua_client_config_t cfg { };
    cfg.server_ip = server_ip;
    cfg.rpc_port = rpc_port;
    cfg.client_name = "capi-smoke";
    cfg.jitter_buffer_slots = 0; // 0 = core 默认
    cfg.hello_interval_ms = 0; // 0 = core 默认
    cfg.playback_frames_per_buffer = 0; // 0 = backend 默认
    cfg.force_udp_port = 0; // 0 = server 通告
    cfg.log_level = -1; // 保持当前级别
    cfg.playback_low_latency = 0; // Android/AAudio: NONE + SHARED 默认
    return cfg;
}

} // namespace

TEST(AquaCapiTest, VersionAndEnumNames)
{
    EXPECT_NE(aqua_version(), nullptr);
    EXPECT_STRNE(aqua_version(), "");

    EXPECT_STREQ(aqua_runtime_state_name(AQUA_STATE_CREATED), "created");
    EXPECT_STREQ(aqua_runtime_state_name(AQUA_STATE_RUNNING), "running");
    EXPECT_STREQ(aqua_runtime_state_name(AQUA_STATE_STOPPED), "stopped");
    EXPECT_STREQ(aqua_runtime_state_name(-1), "unknown");

    EXPECT_STREQ(aqua_audio_error_name(AQUA_AUDIO_NONE), "none");
    EXPECT_STREQ(aqua_audio_error_name(AQUA_AUDIO_FORMAT_UNSUPPORTED), "format_unsupported");
    EXPECT_STREQ(aqua_audio_error_name(999), "unknown");
}

TEST(AquaCapiTest, CreateRejectsInvalidArguments)
{
    EXPECT_EQ(aqua_client_create(nullptr), nullptr);

    auto cfg = make_config("127.0.0.1", 50052);
    cfg.server_ip = nullptr;
    EXPECT_EQ(aqua_client_create(&cfg), nullptr);

    cfg = make_config("", 50052);
    EXPECT_EQ(aqua_client_create(&cfg), nullptr);
}

TEST(AquaCapiTest, LifecycleCreatedToFailedStartToStopped)
{
    // 指向本机无人监听的端口：gRPC connect 失败（阻塞至 core 超时）。
    auto cfg = make_config("127.0.0.1", 59999);
    aqua_client_t* client = aqua_client_create(&cfg);
    ASSERT_NE(client, nullptr);

    // Created 态：状态/诊断/连接结果契约。
    EXPECT_EQ(aqua_client_get_state(client), AQUA_STATE_CREATED);
    EXPECT_EQ(aqua_client_get_last_audio_error(client), AQUA_AUDIO_NONE);

    aqua_client_diagnostics_t diag { };
    ASSERT_EQ(aqua_client_get_diagnostics(client, &diag), AQUA_OK);
    EXPECT_EQ(diag.state, AQUA_STATE_CREATED);
    EXPECT_EQ(diag.net.rx_packets, 0U);
    EXPECT_DOUBLE_EQ(diag.jitter_buffer.water_level, 0.0);
    EXPECT_EQ(diag.playback.pull_calls, 0U);
    EXPECT_EQ(aqua_client_get_diagnostics(client, nullptr), AQUA_ERR_INVALID_ARGUMENT);

    aqua_connect_result_t cr { };
    EXPECT_EQ(aqua_client_get_connect_result(client, &cr), AQUA_ERR_NOT_CONNECTED);

    // 启动失败 → Stopped（一次性生命周期，destroy 释放）。
    EXPECT_EQ(aqua_client_start(client), AQUA_ERR_START_FAILED);
    EXPECT_EQ(aqua_client_get_state(client), AQUA_STATE_STOPPED);

    // stop 幂等；destroy 隐式 stop，不崩溃。
    EXPECT_EQ(aqua_client_stop(client), AQUA_OK);
    aqua_client_destroy(client);
}

TEST(AquaCapiTest, NullHandleQueriesAreSafe)
{
    EXPECT_EQ(aqua_client_get_state(nullptr), -1);
    EXPECT_EQ(aqua_client_get_last_audio_error(nullptr), -1);
    EXPECT_EQ(aqua_client_get_diagnostics(nullptr, nullptr), AQUA_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(aqua_client_get_connect_result(nullptr, nullptr), AQUA_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(aqua_client_start(nullptr), AQUA_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(aqua_client_stop(nullptr), AQUA_ERR_INVALID_ARGUMENT);
    aqua_client_destroy(nullptr); // no-op，不得崩溃
}

// 重复 create/destroy 循环：守护 core logger 只初始化一次（call_once 契约），
// 且 handle 一进一出不泄漏。
TEST(AquaCapiTest, CreateDestroyCycleIsRepeatable)
{
    auto cfg = make_config("127.0.0.1", 59999);
    for (int i = 0; i < 3; ++i) {
        aqua_client_t* client = aqua_client_create(&cfg);
        ASSERT_NE(client, nullptr);
        EXPECT_EQ(aqua_client_get_state(client), AQUA_STATE_CREATED);
        aqua_client_destroy(client);
    }
}

// 起步目标播放设备（playback_device_id）：NULL/空串/有值三种形态 create
// 均接受（设备有效性校验推迟到 start，由 core 回退系统默认兜底）。
TEST(AquaCapiTest, CreateAcceptsInitialPlaybackDevice)
{
    auto cfg = make_config("127.0.0.1", 59999);

    cfg.playback_device_id = nullptr;
    aqua_client_t* c1 = aqua_client_create(&cfg);
    ASSERT_NE(c1, nullptr);
    aqua_client_destroy(c1);

    cfg.playback_device_id = "";
    aqua_client_t* c2 = aqua_client_create(&cfg);
    ASSERT_NE(c2, nullptr);
    aqua_client_destroy(c2);

    cfg.playback_device_id = "android:2";
    aqua_client_t* c3 = aqua_client_create(&cfg);
    ASSERT_NE(c3, nullptr);
    EXPECT_EQ(aqua_client_get_state(c3), AQUA_STATE_CREATED);
    aqua_client_destroy(c3);
}
