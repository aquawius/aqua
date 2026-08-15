/*
 * Aqua — C API（UI ↔ Core 边界）
 *
 * 本头文件是 core 库对外暴露的稳定 C ABI，供 Qt6（C++ extern "C"）与
 * Kotlin/JNI（C）接入。设计目标：
 *   - 只暴露不透明句柄（aqua_client_t* / aqua_server_t*），不泄漏任何 C++ 类布局；
 *   - 所有函数返回 aqua_status_t 状态码（0 = OK，<0 = 错误）；
 *   - 跨边界不抛 C++ 异常，内部 try/catch 全捕获并转成负返回码；
 *   - 跨边界不传递 std::string，字符串统一为 const char*（UTF-8）；
 *   - 字符串入参在 start() 时被拷贝，调用方在 start() 返回后即可释放；
 *   - 回调在 core 内部线程触发（非调用方线程），不得阻塞；需更新 UI 时由调用方自行投递。
 *
 * 线程模型：start() 启动后，core 内部线程完成全部网络/音频工作；
 * shutdown() 请求停止（非阻塞），destroy() 阻塞等待线程收尾并释放。
 * 一个句柄只 start 一次；重复 start 返回 AQUA_ERR_ALREADY_RUNNING。
 */

#ifndef AQUA_H
#define AQUA_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================= 版本 ============================= */

/* 返回库版本字符串，如 "0.0.1"。指针指向静态存储，永远有效。 */
const char* aqua_version(void);

/* ============================= 日志 ============================= */

typedef enum aqua_log_level {
    AQUA_LOG_TRACE = 0,
    AQUA_LOG_DEBUG = 1,
    AQUA_LOG_INFO  = 2,
    AQUA_LOG_WARN  = 3,
    AQUA_LOG_ERROR = 4,
} aqua_log_level_t;

/* 设置全局日志级别。进程级生效（spdlog default logger）。 */
int aqua_set_log_level(aqua_log_level_t level);

/* ============================= 状态码 ============================= */

typedef enum aqua_status {
    AQUA_OK                 = 0,
    AQUA_ERR_INVALID_ARGUMENT = -1, /* 参数为 NULL 或非法 */
    AQUA_ERR_ALREADY_RUNNING   = -2, /* 句柄已 start，重复调用 */
    AQUA_ERR_START_FAILED      = -3, /* 启动失败（详见 last_error） */
    AQUA_ERR_OUT_OF_MEMORY     = -4, /* 内存分配失败 */
    AQUA_ERR_INTERNAL          = -5, /* 内部未知错误 */
} aqua_status_t;

/* ============================= 音频格式 ============================= */

/* 数值必须与 core 内部 aqua::AudioEncoding 一一对应（编译期静态断言校验）。 */
typedef enum aqua_encoding {
    AQUA_ENCODING_INVALID   = 0,
    AQUA_ENCODING_PCM_S16LE = 1,
    AQUA_ENCODING_PCM_S32LE = 2,
    AQUA_ENCODING_PCM_F32LE = 3,
    AQUA_ENCODING_PCM_S24LE = 4,
    AQUA_ENCODING_PCM_U8    = 5,
} aqua_encoding_t;

/* 服务器固定 AudioFormat，只描述音频数据本身，不含传输/缓冲策略。 */
typedef struct aqua_audio_format {
    int32_t  encoding;    /* aqua_encoding_t */
    uint32_t channels;
    uint32_t sample_rate;
} aqua_audio_format_t;

/* ============================= 不透明句柄 ============================= */

typedef struct aqua_client aqua_client_t;
typedef struct aqua_server aqua_server_t;

/* ============================= 客户端 ============================= */

typedef enum aqua_client_state {
    AQUA_CLIENT_IDLE         = 0, /* 未启动 */
    AQUA_CLIENT_CONNECTING   = 1, /* gRPC 连接 + UDP 握手 + 播放初始化中 */
    AQUA_CLIENT_PLAYING      = 2, /* 音频正常播放中 */
    AQUA_CLIENT_RECONNECTING = 3, /* 断线后指数退避等待重连（仅 auto_reconnect） */
    AQUA_CLIENT_STOPPED      = 4, /* 优雅关闭 */
    AQUA_CLIENT_FAILED       = 5, /* 致命错误 */
} aqua_client_state_t;

typedef struct aqua_client_config {
    const char* server_ip;                  /* UTF-8；NULL = "127.0.0.1" */
    uint16_t    server_rpc_port;            /* 0 = 50051 */
    uint32_t    jitter_target_latency_ms;   /* 0 = 默认 30ms */
    uint32_t    jitter_drift_late_threshold;/* 0 = 默认 15 */
    size_t      playback_ringbuffer_size;   /* 0 = 默认 16KB */
    int         auto_reconnect;             /* 0/1，默认 0（断线退出） */
    const char* client_name;                /* UTF-8；NULL = "aqua_client"；仅日志 */
} aqua_client_config_t;

/* 用默认值填充 config。调用方随后可覆盖所需字段再 start()。 */
void aqua_client_config_init(aqua_client_config_t* config);

/* 回调在 core 会话线程触发，不得阻塞。user_data 原样传回，须在句柄生命周期内保持有效。 */
typedef void (*aqua_client_state_cb)(void* user_data, aqua_client_state_t state);
typedef void (*aqua_client_format_cb)(void* user_data, const aqua_audio_format_t* format);
typedef void (*aqua_client_error_cb)(void* user_data, const char* message);
typedef void (*aqua_client_stopped_cb)(void* user_data);

typedef struct aqua_client_callbacks {
    void* user_data;
    aqua_client_state_cb on_state_change; /* 状态迁移 */
    aqua_client_format_cb on_format;      /* 拿到服务器 AudioFormat（每次重连都会触发） */
    aqua_client_error_cb   on_error;      /* 致命错误（进入 Failed） */
    aqua_client_stopped_cb on_stopped;    /* 优雅关闭完成（线程已收尾） */
} aqua_client_callbacks_t;

/* 创建客户端句柄。失败返回 NULL（内存不足）。 */
aqua_client_t* aqua_client_create(void);

/* 销毁句柄：内部先请求停止、阻塞等待线程收尾，再释放。可安全传入 NULL。 */
void aqua_client_destroy(aqua_client_t* client);

/* 启动客户端（非阻塞）：连接、握手、播放、断线重连全部在 core 内部线程进行。
 * 返回 AQUA_OK 表示已启动；运行结果经回调上报。config 必填，callbacks 可为 NULL。 */
int aqua_client_start(aqua_client_t* client,
                      const aqua_client_config_t* config,
                      const aqua_client_callbacks_t* callbacks);

/* 请求优雅关闭（非阻塞）。实际停止在 core 内部线程完成，on_stopped 后 destroy() 可安全调用。 */
int aqua_client_shutdown(aqua_client_t* client);

/* 查询当前状态。client 为 NULL 时返回 AQUA_CLIENT_IDLE。 */
aqua_client_state_t aqua_client_state(const aqua_client_t* client);

/* 最近一次致命错误信息（UTF-8）。指针在下次调用本函数或句柄销毁前有效，调用方应复制。 */
const char* aqua_client_last_error(const aqua_client_t* client);

/* ============================= 服务器 ============================= */

typedef struct aqua_server_config {
    const char* bind_ip;               /* UTF-8；NULL = "0.0.0.0" */
    uint16_t    rpc_port;              /* 0 = 50051 */
    uint16_t    udp_port;              /* 0 = 50000 */
    size_t      capture_ringbuffer_size; /* 0 = 默认 8KB */
} aqua_server_config_t;

/* 用默认值填充 config。 */
void aqua_server_config_init(aqua_server_config_t* config);

typedef void (*aqua_server_started_cb)(void* user_data);
typedef void (*aqua_server_error_cb)(void* user_data, const char* message);
typedef void (*aqua_server_stopped_cb)(void* user_data);

typedef struct aqua_server_callbacks {
    void* user_data;
    aqua_server_started_cb on_started; /* 全部子系统启动完成 */
    aqua_server_error_cb   on_error;   /* 致命错误（已进入关闭流程） */
    aqua_server_stopped_cb on_stopped; /* 优雅关闭完成（线程已收尾） */
} aqua_server_callbacks_t;

/* 创建服务器句柄。失败返回 NULL。 */
aqua_server_t* aqua_server_create(void);

/* 销毁句柄：内部先请求停止、阻塞等待线程收尾，再释放。可安全传入 NULL。 */
void aqua_server_destroy(aqua_server_t* server);

/* 启动服务器。start 阶段同步完成采集/gRPC/UDP 初始化（可能阻塞数百毫秒），
 * 成功返回 AQUA_OK 后内部线程接管运行；失败返回 AQUA_ERR_START_FAILED（详见 last_error）。 */
int aqua_server_start(aqua_server_t* server,
                      const aqua_server_config_t* config,
                      const aqua_server_callbacks_t* callbacks);

/* 请求优雅关闭（非阻塞）。 */
int aqua_server_shutdown(aqua_server_t* server);

/* 最近一次错误信息（UTF-8）。指针有效期同 aqua_client_last_error。 */
const char* aqua_server_last_error(const aqua_server_t* server);

#ifdef __cplusplus
}
#endif

#endif /* AQUA_H */
