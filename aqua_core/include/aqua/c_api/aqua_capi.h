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
//   - 监督逻辑（CLI control timer 的等价物：Degraded → stop；
//     UDP 路径死亡与控制面死亡都致命，经 handler 置 Degraded）
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
//     可被任意线程轮询（Android 侧 500ms 轮询模型；core 内部 io_context 的
//     监督 tick 同为 500ms，见 RUNTIME_CONTROL_POLL_INTERVAL）；
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
    AQUA_ERR_SWITCH_FAILED = 4, // 播放切换事务被拒绝（链耗尽 Fatal 等终态；
                                // 可重试的失败不存在：能走的链事务内已走完，细节见诊断 switch_outcome /
                                // switch_error）。末尾追加，ABI 安全。
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

// aqua::audio::PlaybackRouteMode（playback_switching_design.md §4）
enum aqua_route_mode {
    AQUA_ROUTE_FOLLOW_SYSTEM = 0,
    AQUA_ROUTE_PREFER_CURRENT = 1,
    AQUA_ROUTE_PREFERRED_DEVICE = 2,
};

// aqua::audio::SwitchOutcome（playback_switching_design.md §9）
enum aqua_switch_outcome {
    AQUA_SWITCH_NONE = 0, // 尚未发生切换事务
    AQUA_SWITCH_SWITCHED = 1, // 目标一次成功
    AQUA_SWITCH_ROLLED_BACK = 2, // 目标失败，回滚旧设备成功
    AQUA_SWITCH_FELL_BACK_TO_SYSTEM = 3, // 目标与回滚均失败，落系统默认
    AQUA_SWITCH_FATAL = 4, // 候选链耗尽
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
    uint32_t jb_capacity_slots;
    // 握手期节奏 ms（默认 1000；必须 > 0）。association 建立后
    // 同一包型自动转 HEARTBEAT_INTERVAL 续命节奏（不经此字段）。
    uint32_t heartbeat_handshake_interval_ms;
    // playback 每回调请求帧数（0 = backend 自行决定，WASAPI/AAudio 语义）。
    uint32_t playback_frames_per_buffer;
    // 覆盖 server 通告的 UDP 端口；0 = 采用 server 通告值。
    uint16_t udp_force_port;
    // 日志级别（AQUA_LOG_*）；-1 = 保持进程当前级别不调整。
    int32_t log_level;
    // Android/AAudio 播放低延迟模式：0 = NONE + SHARED，非 0 = LOW_LATENCY + SHARED。
    // 不启用 Exclusive。其它平台忽略。
    int32_t playback_low_latency;
    // 播放路由起步（playback_switching_design.md §4）：0 = FollowSystem
    // （"自动切换播放设备"开，跟随系统默认），非 0 = PreferCurrent（钉住首流
    // 实际设备，不跟随新的系统默认）。路由是连接属性，不持久化。
    int32_t playback_prefer_current;
    // 起步目标播放设备（初始化首流前选定）：NULL/空 = 未指定（按
    // playback_prefer_current 起步）；有值时首流直接在该设备上打开
    // （如 Android "android:N"），起步路由 = PreferredDevice，覆盖
    // playback_prefer_current。设备失效/格式不兼容时首流回退系统默认
    // （连接不因此失败），降级结果经诊断 route_mode 观察。
    const char* playback_device_id;
    // Phase 1 自适应 target（末尾追加）：0 = 自适应开（默认，快启小水位 +
    // 按到达抖动动态调 target）；非 0 = 关闭，用既有固定 target/startup。
    // 连接属性（JB 构造时确定），运行期不可切换。
    int32_t jb_fixed_target;
    // Phase 2 PCM concealment（末尾追加）：0 = 开启（默认，缺帧时重复上一个
    // 有效包 + 短淡出，超 3 包转静音）；非 0 = 关闭，缺帧直接静音（v1 行为）。
    // 连接属性（JB 构造时确定），运行期不可切换。
    int32_t jb_disable_concealment;
    // JB 自适应微调（末尾追加，与 CLI --jb-jitter-gain / --jb-min-target 对齐；
    // 仅 jb_fixed_target=0 自适应开时生效）：
    // jitter 增益 k（margin = k×J，延迟↔稳定的主力旋钮）。0 / 负值 / 非有限
    // = 默认 5.0（zero-init 惯例：0 不应把抖动余量关掉——想要 gain=0 的极值
    // 实验请走 CLI）；极大值由 2/3×capacity 结构上限接住，不会失败。
    double jb_jitter_gain;
    // target 硬下限（slot 数）。0 = 默认 3。有效下限 = max(本值, 几何地板+1)：
    // 只能抬高最低延迟（几何地板无条件托底）；高于 capacity 时被钳到容量。
    uint32_t jb_min_target_slots;
    // ---- JB 现场调优旋钮（末尾追加，ABI 安全；与 CLI --jb-stall-peak-cap /
    // --jb-stall-decay / --jb-stall-threshold / --jb-underrun-penalty 对齐；
    // 仅 jb_fixed_target == 0 自适应开时生效）----
    //
    // zero-init 惯例（与本结构体其它数值旋钮一致）：**0 / 负值 / 非有限 =
    // 采用 core 默认值**（依次为 8.0 槽 / 10.0 ms/s / 5.0 包周期 / 1.0 槽）。
    // Kotlin 侧不填这些字段即可保持既有行为，无需特判。
    //
    // 注意 0 本身的**极值语义**（关闭 stall 峰值项 / 峰值永久保持 / 关闭 stall
    // 检测 / 关闭欠载反馈闭环）只在 CLI 上提供：那是实验入口，产品路径不该因为
    // "字段没填"而静默关掉一层保护机制。语义详见 configuration_reference.md。
    double jb_stall_peak_cap_slots;
    double jb_stall_peak_decay_ms_per_sec;
    double jb_stall_threshold_packets;
    double jb_underrun_penalty_slots;
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
    uint64_t heartbeat_ack_count; // 收到的 HeartbeatAck 总数（建连确认 + 稳态每包回执）
    uint32_t heartbeat_ack_misses; // 当前连续未收到 ACK 的 heartbeat 数（握手期/稳态通用）
    int64_t heartbeat_ack_age_ms; // 距最近一次 ACK 的毫秒数；<0 表示尚未收到 ACK
    uint64_t heartbeat_handshake_send_attempts;
    uint64_t heartbeat_ack_miss_events; // miss tick 累计数（每周期无 ACK +1；字段名历史契约）
    uint64_t audio_frames_accepted; // UDP 侧接受的完整 AudioFrame 数
    uint64_t rx_audio_sequence_gap_events; // 音频接收序列缺口事件数（"收到流缺口"≠"丢包"）
    uint64_t rx_audio_sequence_missing_frames; // 缺口累计缺失帧数
    uint64_t malformed_datagrams;
    uint64_t unexpected_sender_datagrams;
    uint64_t wrong_session_acks;
    uint64_t audio_payload_mismatches;
    uint64_t non_audio_datagrams;
    int32_t heartbeat_failed; // liveness 失败锁存（握手期/稳态任一超限即置 1，只置一次；字段名历史契约）
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
    // ---- Gauge / 当前态（与累计 counter 互补）----
    uint32_t lead_slots; // lead = highest - play + 1（绝对值）
    uint64_t play_sequence; // 播放头序列（未锚定 = 0）
    uint64_t highest_received_sequence; // 已收到的最高序列
    uint64_t consecutive_silence_frames; // 当前连续静音 run
    uint64_t max_silence_run_frames; // 本次运行最长静音 run
    int32_t episode_state; // 0=None 1=Filling 2=Dropping
    int32_t reanchor_pending; // 0/1：有待应用的 reanchor 请求
    uint64_t reanchor_target_sequence; // 待应用目标序列（无 = 0）
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
    // 本次流运行期统计（Gauge：音频线程写、后端缓存读）。
    uint64_t callback_count; // 后端实际回调次数（WASAPI 渲染趟 / AAudio data callback）
    uint32_t current_padding_frames; // 端点缓冲当前填充（WASAPI；AAudio 未知 = 0）
    uint64_t xrun_count; // 欠载/超限（AAudio xRun；WASAPI 暂不统计 = 0）
} aqua_stream_info_t;

// 设备 id 字符串容量：覆盖 Android "android:N"（短）与 WASAPI endpoint id
// （典型 ~78 字符）；超出部分截断（诊断显示用途）。
#define AQUA_DEVICE_ID_BYTES 80

// ---- Buffer 决策层观测（末尾追加，与 aqua::diagnostics::ClientDiagnosticsSnapshot
// 的 jitter_control 一一对应）----
// 上面的 jitter_buffer 组给"结果与累计计数"，本组给"决策与阈值"：回答**为什么**
// target / 水位 / 掩盖是现在这样。组内字段顺序是 Kotlin 侧解码的固定契约。
//
// 本结构体作为 aqua_client_diagnostics_t 的**最后一个**成员存在，因此组内继续
// 追加字段同样 ABI 安全（只增在末尾）。
typedef struct {
    // ---- 决策层：TargetController（仅自适应模式；adaptive=0 时全 0）----
    int32_t adaptive; // 0 = 固定模式（--jb-fixed-target），target 不由 controller 输出
    uint32_t desired_slots; // 未限速期望值；与 target_slots 不等 = 被限速/dwell/死区按住
    uint32_t min_slots; // 生效下限 = max(jb_min_target_slots, 几何地板 + 1)
    uint32_t max_slots; // 结构上限 = 2/3 × jb_capacity_slots
    int32_t margin_source; // margin 胜出方：0=kJ 1=stall_peak
    int32_t path; // 本拍收敛路径：0=steady 1=rise 2=fall 3=dwell_lock 4=deadband 5=no_time_base
    int32_t floor_bound; // 0/1：desired 被下限抬起（margin 失算，安全网托住）
    int32_t cap_bound; // 0/1：desired 被结构上限夹住（正在兜底）
    double underrun_penalty; // 欠载反馈抬升量（slot）；>0 = 闭环在工作（预期，非故障）
    double dwell_remaining_ms; // 涨后锁跌剩余（ms）；>0 = 正在锁跌（峰值保持）
    double fall_room_slots; // 本拍跌侧限速额度（slot）；解释"这一拍为什么只降一格"

    // ---- 观测层尾部：JitterEstimator 的 stall 侧与到达节奏 ----
    uint64_t stall_events; // 被 stall 门剔除的断流次数（不进 J）
    double stall_peak_ms; // 近期最坏到达间隙的衰减最大值（margin 峰值项的输入）
    double last_stall_gap_ms; // 最近一次 stall 的到达间隔
    double arrival_interval_ms; // 最近一个按序包的到达间隔

    // ---- 执行层：水位带（同快照的一组）与当前缺帧/掩盖 run ----
    uint32_t band_warning_low; // 水位带：target 落在带外才 Fill/Drop，故看动作先看带
    uint32_t band_normal_low;
    uint32_t band_normal_high;
    uint32_t band_warning_high;
    uint32_t conceal_run_slots; // 当前连续掩盖 slot 数（0 = 未在掩盖）
    uint32_t underrun_run_slots; // 当前连续缺帧 slot 数（0 = 正常）
} aqua_jitter_control_stats_t;

typedef struct {
    int32_t state; // AQUA_STATE_*
    // 注：音频错误不在快照内（快照 = 组件状态，不承担错误传递）；错误经
    // aqua_client_get_last_audio_error / aqua_client_get_audio_error_epoch
    // 独立通道上报（epoch 变化检测 + 恢复清零语义）。
    int32_t playback_running;
    int32_t playback_state; // AQUA_PLAYBACK_*
    // 播放路由与切换事务（playback_switching_design.md §9）
    int32_t route_mode; // AQUA_ROUTE_*
    int32_t switch_outcome; // AQUA_SWITCH_*
    int32_t switch_error; // AQUA_AUDIO_*（切换链上最后失败原因）
    uint32_t switch_duration_ms; // 最近一次切换事务耗时（ms）
    char requested_device_id[AQUA_DEVICE_ID_BYTES]; // PreferredDevice 请求设备；空串 = 无
    char stream_device_id[AQUA_DEVICE_ID_BYTES]; // 实际输出设备回读；空串 = 未知
    aqua_net_stats_t net;
    aqua_jitter_buffer_stats_t jitter_buffer;
    aqua_playback_stats_t playback;
    aqua_stream_info_t stream;
    // Phase 0 网络观测（JitterEstimator 纯观察；末尾追加，老字段位置不动）。
    // double 以位模式过 JNI（writeF64），与 water_level 同机制。
    double estimator_jitter_ms; // RFC 3550 interarrival jitter（观测量 ≠ target）
    double estimator_base_delay_ms; // 路径底噪
    double estimator_transit_ms; // 当前相对 transit
    uint64_t estimator_reordered_packets; // 乱序到达
    uint64_t estimator_duplicate_packets; // 重复到达
    uint64_t estimator_late_packets; // 落后观测窗之外
    // Phase 1 自适应 target（末尾追加）：与实际 lead、estimator jitter
    // 同一快照可读，可解释 target 为什么变化、JB 为什么没达到 target。
    uint32_t target_slots; // 当前 target（固定模式 = 构造值；自适应 = controller 输出）
    double target_ms; // target 换算毫秒
    // Phase 2 欠载预算 + PCM concealment（末尾追加）。underrun = 播放头推进到
    // 没有真实 PCM 可用的 slot（conceal 掩盖帧也算），不含 pre-roll/Hold 静音。
    uint64_t underrun_events; // 缺帧 run 次数
    uint64_t underrun_frames; // 掩盖帧 + 缺帧静音帧
    uint64_t max_consecutive_underrun_slots; // 单次最长缺帧 slot 数
    uint64_t concealed_slots; // repeat-last 掩盖的 slot 数
    uint64_t concealed_saturated_slots; // 超上限退回静音的 slot 数
    uint64_t late_useful_packets; // 迟到但落在 conceal 窗口内（本可用）的包数
    double underrun_ratio; // underrun_frames / pull_frames
    double fill_duty; // Fill 慢放多播帧占比
    double drop_duty; // Drop 跳过 slot 帧占比
    // 细则 §11：lead_slots + lead_ms + target + jitter 同快照可读（末尾追加）。
    double lead_ms; // 实际 lead 换算毫秒（与 target_ms 同口径）
    // Buffer 决策层观测（末尾追加）。本成员位于结构体末尾，故 aqua_jitter_control_stats_t
    // 内部继续追加字段同样 ABI 安全；写入顺序见 aqua_jni.cpp 的 nativeGetDiagnostics。
    aqua_jitter_control_stats_t jitter_control;
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
    // 动态值：当前学到的实际对端（HeartbeatAck 来源），建连时确定。
    // address 为空串表示尚未学到（握手前）。
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
// 单飞：监督线程在跑时重复调用返回 AQUA_ERR_INVALID_ARGUMENT；
// 已停止的 handle 重调返回 AQUA_ERR_START_FAILED（runtime 一次性，不可复用）。
int aqua_client_start(aqua_client_t* client);

// 停止（幂等）：停止 runtime、断开 gRPC、join IO 线程。
int aqua_client_stop(aqua_client_t* client);

// 销毁并释放 handle（含隐式 stop）。传 NULL 安全。
void aqua_client_destroy(aqua_client_t* client);

// ---- 查询（线程安全，供轮询）----

// 返回 AQUA_STATE_*；handle 为 NULL 时返回 -1。
int aqua_client_get_state(const aqua_client_t* client);

// 返回当前 audio 错误（AQUA_AUDIO_*；错误通道，非诊断快照字段）：
// AQUA_AUDIO_NONE = 当前无未恢复错误（成功的恢复事务会清零，不再残留）。
// handle 为 NULL 时返回 -1。
int aqua_client_get_last_audio_error(const aqua_client_t* client);

// 返回 audio 错误事件纪元：错误每次变化（置位新值 / 恢复清零）递增。
// 轮询方以 epoch 变化检测错误事件——既能看到新错误，也能看到"已恢复"
// （epoch 变 + get_last_audio_error == NONE）。handle 为 NULL 时返回 0。
uint64_t aqua_client_get_audio_error_epoch(const aqua_client_t* client);

// 填充诊断快照。out 为 NULL 或 handle 非法返回 AQUA_ERR_INVALID_ARGUMENT。
int aqua_client_get_diagnostics(const aqua_client_t* client,
    aqua_client_diagnostics_t* out);

// ---- 播放设备切换（playback_switching_design.md §9）----

// 显式切换播放设备（用户选择）。device_id == NULL 表示跟随系统
// （FollowSystem）；否则为后端格式 id（Android = "android:N"，由 JNI 编码，
// Kotlin 不做字符串拼接）。
// 同步执行完整候选链（target -> previous -> system_default），返回时事务已完成，
// 结果经诊断的 switch_outcome / switch_error 观察（驱动 UI 降级横幅）。
// 返回：AQUA_OK = 事务完成（含降级成功）；AQUA_ERR_NOT_CONNECTED = 未连接；
// AQUA_ERR_INVALID_ARGUMENT = 参数非法；AQUA_ERR_SWITCH_FAILED = 链耗尽等终态拒绝。
int aqua_client_set_playback_device(aqua_client_t* client, const char* device_id);

// 播放设备集合变化推送（playback_switching_design.md §5 rev2，平台推送模型）：
// present_ids = 当前可选输出设备 id 全集（后端词汇：Android = "android:N"，
// 由 JNI 编码；WASAPI = endpoint id），count = 元素数（0 / NULL = 空集）。
// core 内部做 1s 合并去抖（最新快照胜出），随后按路由模式完成全部决策
// （FollowSystem 跟随新设备 / 活跃设备消失 eager 回退 / PreferredDevice
// 自动切回）；调用方只转发事件，不做任何路由决策。
// 设备发现留在平台层（Android = Kotlin AudioManager），core 不建设备注册表。
// 线程安全，可在任意线程调用；未连接 / 非运行时为 no-op（快照作基线）。
// 异常不越过 C 边界：非法 count 或分配失败时静默丢弃本批快照。
void aqua_client_notify_devices_changed(aqua_client_t* client,
    const char* const* present_ids, int32_t count) noexcept;

// 填充连接结果（音频契约）。start 成功前返回 AQUA_ERR_NOT_CONNECTED。
int aqua_client_get_connect_result(const aqua_client_t* client,
    aqua_connect_result_t* out);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // AQUA_C_API_AQUA_CAPI_H
