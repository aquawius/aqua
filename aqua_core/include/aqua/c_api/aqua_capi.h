#ifndef AQUA_C_API_AQUA_CAPI_H
#define AQUA_C_API_AQUA_CAPI_H

// Aqua client C API：为 JNI / Android（以及其他 GUI 前端）提供的稳定 C 边界，
// 产出独立共享库（aqua_capi 目标，产物名 aqua：libaqua.so / aqua.dll）。
// Android 交叉编译后拷贝进 app 工程的 jniLibs 即可使用。
//
// 设计约束（见 aqua_core/doc/android_roadmap.md §4）：
//   - 只暴露 opaque handle 与纯 C 类型，不泄漏 C++ STL 类型；
//   - 业务全部由 aqua::runtime::ClientRuntime 实现，本 API 是薄 wrapper，
//     不是第二个 runtime；
//   - 监督逻辑（CLI control timer 的等价物：Degraded / hello_failed -> stop）
//     由内部 IO 线程执行，与 aqua_client_cli 语义一致。
//
// 生命周期（一次性，与 ClientRuntime 相同）：
//   aqua_client_create -> (Created)
//   aqua_client_start  -> (Starting -> Running/Degraded)   [阻塞，至 gRPC 超时]
//   aqua_client_stop   -> (Stopping -> Stopped)
//   aqua_client_destroy
//
// 线程约定：
//   - create/start/stop/destroy 必须由同一个控制线程按序调用（同 ClientRuntime）；
//   - 查询类 API（get_state/get_diagnostics/get_audio_format/version）线程安全，
//     可被任意线程轮询（Android 侧 250ms 轮询模型）；
//   - start() 内部会拉起 IO/监督线程；stop()/destroy() 会停止并 join 它。

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---- 版本 ----

// 返回 core 版本字符串（AQUA_CORE_VERSION，静态存储，勿释放）。
const char* aqua_version(void);

// ---- 错误码（C API 自身的调用结果）----

enum {
    AQUA_OK = 0,
    AQUA_ERR_INVALID_ARGUMENT = 1, // handle/config/输出指针非法
    AQUA_ERR_START_FAILED = 2, // ClientRuntime::start() 失败（细节见日志）
    AQUA_ERR_NOT_CONNECTED = 3, // 尚未成功连接（音频格式等不可用）
};

// ---- 枚举镜像（数值与 aqua core C++ 枚举的声明顺序一一对应，禁止改动）----

// aqua::runtime::RuntimeState
enum aqua_runtime_state {
    AQUA_STATE_CREATED = 0,
    AQUA_STATE_STARTING = 1,
    AQUA_STATE_RUNNING = 2,
    AQUA_STATE_DEGRADED = 3,
    AQUA_STATE_STOPPING = 4,
    AQUA_STATE_STOPPED = 5,
};

// aqua::audio::PlaybackState（本地播放生命的平行状态维度，
// playback_switching_design.md §3；Fatal = restart fallback 链耗尽终态）
enum aqua_playback_state {
    AQUA_PLAYBACK_INACTIVE = 0,
    AQUA_PLAYBACK_STARTING = 1,
    AQUA_PLAYBACK_RUNNING = 2,
    AQUA_PLAYBACK_SWITCHING = 3,
    AQUA_PLAYBACK_FATAL = 4,
};

// aqua::audio::AudioError
enum aqua_audio_error {
    AQUA_AUDIO_NONE = 0,
    AQUA_AUDIO_DEVICE_NOT_FOUND = 1,
    AQUA_AUDIO_DEVICE_UNAVAILABLE = 2,
    AQUA_AUDIO_DEVICE_DISCONNECTED = 3,
    AQUA_AUDIO_FORMAT_UNSUPPORTED = 4,
    AQUA_AUDIO_NOT_SUPPORTED = 5,
    AQUA_AUDIO_PERMISSION_DENIED = 6,
    AQUA_AUDIO_ALREADY_RUNNING = 7,
    AQUA_AUDIO_NOT_RUNNING = 8,
    AQUA_AUDIO_INVALID_ARGUMENT = 9,
    AQUA_AUDIO_BACKEND_FAILED = 10,
};

// 日志级别：aqua::LogLevel
enum aqua_log_level {
    AQUA_LOG_TRACE = 0,
    AQUA_LOG_DEBUG = 1,
    AQUA_LOG_INFO = 2,
    AQUA_LOG_WARN = 3,
    AQUA_LOG_ERROR = 4,
    AQUA_LOG_FATAL = 5,
};

// 枚举名查询（静态存储，勿释放）；数值非法时返回 "unknown"。
const char* aqua_runtime_state_name(int state);
const char* aqua_audio_error_name(int error);

// ---- 配置 ----

typedef struct {
    // 目标 server（IP 字面量，IPv4/IPv6；不支持 DNS 主机名）。必填。
    const char* server_ip;
    // gRPC 控制面端口（默认 50051）。
    uint16_t rpc_port;
    // client 显示名（默认 "aqua-client"）。可空指针。
    const char* client_name;
    // JitterBuffer 容量（slot 数，默认 30）。
    uint32_t jitter_buffer_slots;
    // HELLO 保活间隔 ms（默认 1000；必须 > 0）。
    uint32_t hello_interval_ms;
    // playback 每回调请求帧数（0 = backend 自行决定，WASAPI/AAudio 语义）。
    uint32_t playback_frames_per_buffer;
    // 覆盖 server 通告的 UDP 端口；0 = 采用 server 通告值。
    uint16_t force_udp_port;
    // 日志级别（AQUA_LOG_*）；-1 = 保持进程当前级别不调整。
    int32_t log_level;
    // Android/AAudio 播放低延迟模式：0 = NONE + SHARED，非 0 = LOW_LATENCY + SHARED。
    // 不启用 Exclusive。其它平台忽略。
    int32_t playback_low_latency;
} aqua_client_config_t;

// ---- 诊断快照（字段与 aqua::diagnostics::ClientDiagnosticsSnapshot 一一对应）----
// 仅供监控/显示：各字段为原子近似读值，多线程并发下不保证一致。
// 字段分组与顺序是 Kotlin 侧解码的固定契约（见 AquaNative 文档）。

typedef struct {
    uint64_t rx_packets; // 成功收到的 datagram 数
    uint64_t rx_bytes;
    uint64_t rx_errors;
    uint64_t tx_packets;
    uint64_t tx_bytes;
    uint64_t tx_errors;
    uint64_t tx_dropped; // 发送队列超限丢弃
    uint64_t tx_enqueue_failures;
    uint64_t tx_queue_depth;
    uint64_t hello_ack_count; // 收到的 HELLO_ACK 总数
    uint32_t hello_ack_misses; // 当前连续未 ACK 的 HELLO 数
    int64_t hello_ack_age_ms; // 距最近一次 ACK 的毫秒数；<0 表示尚未收到 ACK
    uint64_t hello_send_attempts;
    uint64_t hello_ack_miss_events;
    uint64_t audio_frames_accepted; // UDP 侧接受的完整 AudioFrame 数
    uint64_t malformed_datagrams;
    uint64_t unexpected_sender_datagrams;
    uint64_t wrong_session_acks;
    uint64_t audio_payload_mismatches;
    uint64_t non_audio_datagrams;
    int32_t hello_failed; // HELLO 保活已判定失败（终态）
} aqua_net_stats_t;

typedef struct {
    double water_level; // W = lead_slots / N
    uint32_t used_slots;
    uint32_t capacity_slots;
    uint64_t reanchor_count;
    uint64_t reanchor_requests;
    uint64_t reanchor_cancels;
    uint64_t reanchor_sanity_rejections;
    uint64_t last_reanchor_sequence;
    uint64_t push_accepted;
    uint64_t push_rejected;
    uint64_t push_rejected_late;
    uint64_t push_rejected_slot_busy;
    uint64_t push_rejected_invalid;
    uint64_t push_rejected_sanity;
    uint64_t pull_calls;
    uint64_t pull_frames;
    uint64_t pull_silence_frames;
    uint64_t fill_episodes;
    uint64_t fill_corrected_slots;
    uint64_t drop_episodes;
    uint64_t drop_skipped_slots;
} aqua_jitter_buffer_stats_t;

typedef struct {
    uint64_t pull_calls;
    uint64_t pull_frames;
    uint64_t pull_silence_frames;
} aqua_playback_stats_t;

// playback 输出流实际运行参数（后端 open 后回读；backend=0 表示未运行/未知）。
// 取值语义见 aqua::audio::AudioStreamInfo（audio_playback.h）：
//   backend: 0=none 1=AAudio 2=WASAPI；
//   performance_mode: 10=none 11=power_saving 12=low_latency
//     （AAudio 为 AAUDIO_PERFORMANCE_MODE_* 原值；WASAPI 12=IAudioClient3, 10=legacy）；
//   buffer_capacity_frames: 缓冲容量（帧）；本项目策略 = 永远填满设备缓冲，
//     故不采集 buffer_size（AAudio 不调 setBufferSizeInFrames 时 size 恒等于容量）；
//   不采集 sharing_mode（仅 SHARED）与 callback_frames（AAudio 未设
//     setFramesPerCallback 时回读恒为 unspecified）。
typedef struct {
    uint32_t backend;
    uint32_t sample_rate;
    uint32_t channels;
    int32_t performance_mode;
    uint32_t frames_per_burst;
    uint32_t buffer_capacity_frames;
} aqua_stream_info_t;

typedef struct {
    int32_t state; // AQUA_STATE_*
    int32_t last_audio_error; // AQUA_AUDIO_*
    int32_t playback_running;
    int32_t playback_state; // AQUA_PLAYBACK_*
    aqua_net_stats_t net;
    aqua_jitter_buffer_stats_t jitter_buffer;
    aqua_playback_stats_t playback;
    aqua_stream_info_t stream;
} aqua_client_diagnostics_t;

// ---- 连接结果（start 成功后有效）----

typedef struct {
    uint32_t session_id;
    char advertised_udp_address[64]; // 服务端通告的数据面地址（点分 IPv4 / IPv6 字面量）
    uint16_t advertised_udp_port;
    int32_t audio_encoding; // aqua::audio::AudioEncoding 数值（1=PCM_S16LE .. 5=PCM_U8）
    uint32_t channels;
    uint32_t sample_rate;
    uint32_t frame_count; // 每 AudioFrame 的 sample frame 数 F
    // 动态值：当前学到的实际对端（HELLO_ACK 来源），每次有效 HELLO_ACK 刷新为 sender，
    // 不是一次性初始化参数；address 为空串表示尚未学到。
    char learned_udp_address[64];
    uint16_t learned_udp_port;
} aqua_connect_result_t;

// ---- client handle ----

typedef struct aqua_client aqua_client_t;

// 创建 client（不连接）。config 为 NULL 时返回 NULL。
// 首次调用会初始化 core 日志（Android = logcat sink；其他平台 = stdout）。
aqua_client_t* aqua_client_create(const aqua_client_config_t* config);

// 启动（阻塞至 gRPC Connect 完成，超时由 core 决定）。
// 成功返回 AQUA_OK 并拉起内部 IO/监督线程；
// 失败返回 AQUA_ERR_START_FAILED（handle 进入 Stopped 态，只能 destroy 重建）。
int aqua_client_start(aqua_client_t* client);

// 停止（幂等）：停止 runtime、断开 gRPC、join IO 线程。
int aqua_client_stop(aqua_client_t* client);

// 销毁并释放 handle（含隐式 stop）。传 NULL 安全。
void aqua_client_destroy(aqua_client_t* client);

// ---- 查询（线程安全，供轮询）----

// 返回 AQUA_STATE_*；handle 为 NULL 时返回 -1。
int aqua_client_get_state(const aqua_client_t* client);

// 返回最近一次 audio 错误（AQUA_AUDIO_*）；handle 为 NULL 时返回 -1。
int aqua_client_get_last_audio_error(const aqua_client_t* client);

// 填充诊断快照。out 为 NULL 或 handle 非法返回 AQUA_ERR_INVALID_ARGUMENT。
int aqua_client_get_diagnostics(const aqua_client_t* client,
    aqua_client_diagnostics_t* out);

// 填充连接结果（音频契约）。start 成功前返回 AQUA_ERR_NOT_CONNECTED。
int aqua_client_get_connect_result(const aqua_client_t* client,
    aqua_connect_result_t* out);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // AQUA_C_API_AQUA_CAPI_H
