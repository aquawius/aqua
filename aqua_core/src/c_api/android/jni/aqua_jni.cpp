// Aqua Android JNI 桥：动态注册，映射 com.aquawius.aqua.native.AquaNative。
//
// 契约与 AquaNative.kt 文档一致：
// - diagnostics: LongArray(111)，字段顺序 = aqua_client_diagnostics_t 扁平化
//   （state, playback_running, playback_state, route_mode,
//   switch_outcome, switch_error 先，net/jb/playback/stream 分组随后，
//   每组内按结构体声明顺序）；uint64 -> Long（值直传，非位重解释）。
//   音频错误不在快照内：错误通道 = nativeGetLastAudioError +
//   nativeGetAudioErrorEpoch（epoch 变化检测 + 恢复清零语义）。
// - connectResult: IntArray(7) {sessionId, advertisedUdpPort, encoding, channels,
//   sampleRate, frameCount, learnedUdpPort}；未连接时返回 null；
// - nativeCreate 末两参数 playbackLowLatency / playbackPreferCurrent：
//   false = NONE + SHARED / FollowSystem，true = LOW_LATENCY + SHARED / PreferCurrent。
//   追加参数 initialDeviceId：起步目标播放设备（首流初始化前选定），
//   -1 = 未指定；否则编码 "android:N"，起步路由 = PreferredDevice（覆盖
//   playbackPreferCurrent），设备失效时首流回退系统默认。
// - nativeCreate 末尾追加 6 个 JB 调优参数（与 CLI 同名项对齐；语义一律沿用
//   C API 的 zero-init 惯例：**0 / 负值 = core 默认**，0 的"关闭该机制"极值只在
//   CLI 提供）：jitterGain(double) / minTargetSlots(int) / stallPeakCap(double) /
//   stallPeakDecayMsPerSec(double) / stallThresholdPackets(double) /
//   underrunPenaltySlots(double)。
//   advertisedUdpAddress / learnedUdpAddress 单独查询（String）。
// - 设备路由（playback_switching_design.md §9）：
//   nativeSetPlaybackDevice(handle, int deviceId)：-1 = 跟随系统；否则编码为
//   "android:N"（Kotlin 无字符串拼接）；设备 id 字符串经
//   nativeGetPlaybackDeviceIds 查询（Array(2)：[requested, stream]，空串 = 无）。
// - 设备集合推送（playback_switching_design.md §5 rev2）：
//   nativeNotifyDevicesChanged(handle, IntArray)：当前可选输出设备 id 全集，
//   JNI 编码 "android:N"；core 内部合并去抖 + 路由决策，Kotlin 只转发。
//
// 线程模型：与 C API 一致——create/start/stop/destroy 由控制线程串行；
// 查询可任意线程轮询（App 侧 500ms 轮询 + 500ms Service 循环；core 内部
// io_context 另有 500ms 监督 tick，见 RUNTIME_CONTROL_POLL_INTERVAL）。

#include <jni.h>

#include "aqua/c_api/aqua_capi.h"
#include "aqua/net/address/address_utils.h"

#include "../../aqua_capi_internal.h"

#include <android/log.h>

#include <array>
#include <bit>
#include <cstdint>
#include <cstdio>
#include <iterator>
#include <string>
#include <vector>

namespace {

constexpr char kTagAqua[] = "aqua";

// diagnostics LongArray 的字段数（真相源在 C 头，与结构体相邻——加字段忘改
// 计数会在那里被看到；此处另有一次运行时 mismatch 日志兜底）。
constexpr std::size_t kDiagnosticsCount = AQUA_DIAGNOSTICS_FIELD_COUNT;

// GetStringUTFChars / ReleaseStringUTFChars 的 RAII：任何退出路径都 Release。
// jstring 为 nullptr 时 chars_ 为 nullptr（与旧手工写法同语义）。
class UtfChars {
public:
    UtfChars(JNIEnv* env, jstring str) noexcept
        : env_(env)
        , str_(str)
        , chars_(str != nullptr ? env->GetStringUTFChars(str, nullptr) : nullptr)
    {
    }
    ~UtfChars()
    {
        if (chars_ != nullptr) {
            env_->ReleaseStringUTFChars(str_, chars_);
        }
    }
    UtfChars(const UtfChars&) = delete;
    UtfChars& operator=(const UtfChars&) = delete;

    [[nodiscard]] const char* get() const noexcept { return chars_; }
    [[nodiscard]] explicit operator bool() const noexcept { return chars_ != nullptr; }

private:
    JNIEnv* env_;
    jstring str_;
    const char* chars_;
};

// jlong 来自 Java，可被伪造或引用已 destroy 的实例：统一经 magic 浅校验
// （语义与局限见 aqua_capi_internal.h）。
aqua_client_t* as_client(jlong handle) noexcept
{
    auto* client = reinterpret_cast<aqua_client_t*>(handle);
    return aqua_client_handle_valid(client) ? client : nullptr;
}

// ---- 诊断字段写入（纯 C++ 侧填充，noexcept）----
// 旧实现逐个 SetLongArrayRegion 共 111 次 JNI 调用且无 ExceptionCheck——
// 第 k 次写入抛出 pending 异常后继续调 JNI 是 UB。现在先在 C++ 侧填满
// 整个数组，最后一次 SetLongArrayRegion 提交：UB 面消失，JNI 调用 111 → 1。
using DiagnosticsArray = std::array<jlong, kDiagnosticsCount>;

void writeLong(DiagnosticsArray& out, std::size_t& index, jlong value) noexcept
{
    out[index++] = value;
}

void writeU64(DiagnosticsArray& out, std::size_t& index, std::uint64_t value) noexcept
{
    // uint64 计数器 ≤ 2^63-1 的量级（时间×速率远不可达），值直传。
    writeLong(out, index, static_cast<jlong>(value));
}

void writeI64(DiagnosticsArray& out, std::size_t& index, std::int64_t value) noexcept
{
    writeLong(out, index, static_cast<jlong>(value));
}

void writeI32(DiagnosticsArray& out, std::size_t& index, std::int32_t value) noexcept
{
    writeLong(out, index, static_cast<jlong>(value));
}

void writeF64(DiagnosticsArray& out, std::size_t& index, double value) noexcept
{
    // double → jlong 位重解释（bit_cast 替代 memcpy；尺寸不同则编译失败）。
    writeLong(out, index, std::bit_cast<jlong>(value));
}

// ---- 动态注册表 ----

jlong nativeCreate(JNIEnv* env, jobject, jstring server_ip, jint rpc_port,
    jstring client_name, jint jb_capacity, jint heartbeat_handshake_interval_ms,
    jint playback_frames, jint udp_force_port, jint log_level,
    jboolean playback_low_latency, jboolean playback_prefer_current,
    jint initial_device_id, jdouble jitter_gain, jint min_target_slots,
    jdouble stall_peak_cap, jdouble stall_peak_decay_ms_per_sec,
    jdouble stall_threshold_packets, jdouble underrun_penalty_slots)
{
    if (server_ip == nullptr) {
        return 0;
    }
    const UtfChars server_ip_utf(env, server_ip);
    if (!server_ip_utf) {
        return 0; // OOM 已抛出
    }
    // client_name 可为 nullptr：UtfChars 对空 jstring 产出空指针，与旧写法同语义。
    const UtfChars client_name_utf(env, client_name);
    if (client_name != nullptr && !client_name_utf) {
        return 0; // OOM 已抛出
    }

    aqua_client_config_t config { };
    config.server_ip = server_ip_utf.get();
    // JNI jint 可为负（Kotlin 之外直调）：钳制到"0 = 默认/通告语义"，与 C API 头契约对齐；
    // 负握手间隔会变成巨大 interval 导致保活停摆，负端口会回绕到 65535。
    config.rpc_port = (rpc_port > 0 && rpc_port <= 65535) ? static_cast<std::uint16_t>(rpc_port) : 0;
    config.client_name = client_name_utf.get();
    config.jb_capacity_slots = jb_capacity > 0 ? static_cast<std::uint32_t>(jb_capacity) : 0;
    config.heartbeat_handshake_interval_ms = heartbeat_handshake_interval_ms > 0 ? static_cast<std::uint32_t>(heartbeat_handshake_interval_ms) : 0;
    config.playback_frames_per_buffer = playback_frames > 0 ? static_cast<std::uint32_t>(playback_frames) : 0;
    config.udp_force_port = (udp_force_port > 0 && udp_force_port <= 65535)
        ? static_cast<std::uint16_t>(udp_force_port)
        : 0;
    config.log_level = log_level; // -1 = 保持进程当前级别
    config.playback_low_latency = playback_low_latency == JNI_TRUE ? 1 : 0;
    config.playback_prefer_current = playback_prefer_current == JNI_TRUE ? 1 : 0;
    // 起步目标播放设备（初始化首流前选定）：<0 = 未指定；否则编码
    // "android:N"（与 nativeSetPlaybackDevice 同一词汇，Kotlin 不做字符串拼接）。
    char initial_device[32];
    if (initial_device_id >= 0) {
        std::snprintf(initial_device, sizeof(initial_device), "android:%d",
            static_cast<int>(initial_device_id));
        config.playback_device_id = initial_device;
    }

    // JB 调优（CLI 同名项）：直传即可——C API 侧统一按 "0 / 负 / 非有限 =
    // core 默认" 归一，Kotlin 只需保证"默认"用 0 表示。min_target_slots 是
    // uint32，先把负 jint 挡成 0（否则会回绕成 40 亿，被 core 判为非法容量）。
    config.jb_jitter_gain = jitter_gain;
    config.jb_min_target_slots
        = min_target_slots > 0 ? static_cast<std::uint32_t>(min_target_slots) : 0;
    config.jb_stall_peak_cap_slots = stall_peak_cap;
    config.jb_stall_peak_decay_ms_per_sec = stall_peak_decay_ms_per_sec;
    config.jb_stall_threshold_packets = stall_threshold_packets;
    config.jb_underrun_penalty_slots = underrun_penalty_slots;

    aqua_client_t* client = aqua_client_create(&config);
    return reinterpret_cast<jlong>(client);
}

jint nativeStart(JNIEnv*, jobject, jlong handle)
{
    return aqua_client_start(as_client(handle));
}

jint nativeStop(JNIEnv*, jobject, jlong handle)
{
    return aqua_client_stop(as_client(handle));
}

void nativeDestroy(JNIEnv*, jobject, jlong handle)
{
    aqua_client_destroy(as_client(handle));
}

jint nativeGetState(JNIEnv*, jobject, jlong handle)
{
    return aqua_client_get_state(as_client(handle));
}

jint nativeGetLastAudioError(JNIEnv*, jobject, jlong handle)
{
    return aqua_client_get_last_audio_error(
        as_client(handle));
}

jlong nativeGetAudioErrorEpoch(JNIEnv*, jobject, jlong handle)
{
    return static_cast<jlong>(aqua_client_get_audio_error_epoch(
        as_client(handle)));
}

jstring nativeGetLastErrorName(JNIEnv* env, jobject, jlong handle)
{
    const int error = aqua_client_get_last_audio_error(
        as_client(handle));
    return env->NewStringUTF(aqua_audio_error_name(error));
}

// ---- diagnostics: LongArray(111) ----
// 顺序契约（与 aqua_client_diagnostics_t 声明顺序一一对应，Kotlin 侧
// AquaDiagnostics.fromArray 按同一顺序解码并校验 size == 111）：
// [0..6]     头部 7 项：state, playback_running, playback_state,
//            route_mode, switch_outcome, switch_error, switch_duration_ms
// [7..29]    net 分组 23 项（transport 9 + heartbeat 5 + 分类 9：含音频序列缺口）
// [30..58]   jitter_buffer 分组 29 项（21 累计 + 8 gauge：lead_slots,
//            play_sequence, highest_received_sequence,
//            consecutive_silence_frames, max_silence_run_frames,
//            episode_state, reanchor_pending, reanchor_target_sequence）
// [59..61]   playback 分组 3 项
// [62..70]   stream 分组 9 项（6 参数 + 3 运行期统计：callback_count,
//            current_padding_frames, xrun_count）
// [71..76]   Phase 0 网络观测 6 项（estimator jitter/base/transit/reord/dup/late）
// [77..78]   Phase 1 自适应 target 2 项（target_slots, target_ms）
// [79..87]   Phase 2 欠载预算 + concealment 9 项（underrun_events/frames/
//            max_consecutive_slots, concealed/saturated slots, late_useful,
//            underrun_ratio, fill_duty, drop_duty）
// [88]       lead_ms（细则 §11：lead 与 target/jitter 同快照）
// [89..109]  Buffer 决策层观测 21 项（jitter_control 组，与 aqua_capi.h 的
//            aqua_jitter_control_stats_t 声明顺序一致）：
//              决策层 11：adaptive, desired_slots, min_slots, max_slots,
//                margin_source, path, floor_bound, cap_bound,
//                underrun_penalty, dwell_remaining_ms, fall_room_slots
//              观测层尾部 4：stall_events, stall_peak_ms, last_stall_gap_ms,
//                arrival_interval_ms
//              执行层 6：band_warning_low, band_normal_low, band_normal_high,
//                band_warning_high, conceal_run_slots, underrun_run_slots
// [110]       switch_seq（播放设备切换事务序号，每笔事务递增）
//
// 增删 C++ 诊断字段时必须同步本文件与 Kotlin 解码；kDiagnosticsCount 是硬编码，
// 只有运行时的 mismatch 日志兜底——不一致时 Kotlin 会静默返回 null（UI 停在
// "正在收集数据…"），因此改动后务必真机确认一次。
jlongArray nativeGetDiagnostics(JNIEnv* env, jobject, jlong handle)
{
    auto* client = as_client(handle);
    if (client == nullptr) {
        return nullptr;
    }

    aqua_client_diagnostics_t diag { };
    if (aqua_client_get_diagnostics(client, &diag) != AQUA_OK) {
        return nullptr;
    }

    // 先在 C++ 侧填满（纯 C++，noexcept），最后一次 SetLongArrayRegion 提交。
    DiagnosticsArray values { };
    std::size_t i = 0;
    writeI32(values, i, diag.state);
    writeI32(values, i, diag.playback_running);
    writeI32(values, i, diag.playback_state);
    writeI32(values, i, diag.route_mode);
    writeI32(values, i, diag.switch_outcome);
    writeI32(values, i, diag.switch_error);
    writeI32(values, i, static_cast<std::int32_t>(diag.switch_duration_ms));

    // net 分组（声明顺序）
    writeU64(values, i, diag.net.rx_packets);
    writeU64(values, i, diag.net.rx_bytes);
    writeU64(values, i, diag.net.rx_errors);
    writeU64(values, i, diag.net.tx_packets);
    writeU64(values, i, diag.net.tx_bytes);
    writeU64(values, i, diag.net.tx_errors);
    writeU64(values, i, diag.net.tx_dropped);
    writeU64(values, i, diag.net.tx_enqueue_failures);
    writeU64(values, i, diag.net.tx_queue_depth);
    writeU64(values, i, diag.net.heartbeat_ack_count);
    writeI32(values, i, static_cast<std::int32_t>(diag.net.heartbeat_ack_misses));
    writeI64(values, i, diag.net.heartbeat_ack_age_ms);
    writeU64(values, i, diag.net.heartbeat_handshake_send_attempts);
    writeU64(values, i, diag.net.heartbeat_ack_miss_events);
    writeU64(values, i, diag.net.audio_frames_accepted);
    writeU64(values, i, diag.net.rx_audio_sequence_gap_events);
    writeU64(values, i, diag.net.rx_audio_sequence_missing_frames);
    writeU64(values, i, diag.net.malformed_datagrams);
    writeU64(values, i, diag.net.unexpected_sender_datagrams);
    writeU64(values, i, diag.net.wrong_session_acks);
    writeU64(values, i, diag.net.audio_payload_mismatches);
    writeU64(values, i, diag.net.non_audio_datagrams);
    writeI32(values, i, diag.net.heartbeat_failed);

    // jitter_buffer 分组（声明顺序）
    writeF64(values, i, diag.jitter_buffer.water_level);
    writeI32(values, i, diag.jitter_buffer.used_slots);
    writeI32(values, i, diag.jitter_buffer.capacity_slots);
    writeU64(values, i, diag.jitter_buffer.reanchor_count);
    writeU64(values, i, diag.jitter_buffer.reanchor_requests);
    writeU64(values, i, diag.jitter_buffer.reanchor_cancels);
    writeU64(values, i, diag.jitter_buffer.reanchor_sanity_rejections);
    writeU64(values, i, diag.jitter_buffer.last_reanchor_sequence);
    writeU64(values, i, diag.jitter_buffer.push_accepted);
    writeU64(values, i, diag.jitter_buffer.push_rejected);
    writeU64(values, i, diag.jitter_buffer.push_rejected_late);
    writeU64(values, i, diag.jitter_buffer.push_rejected_slot_busy);
    writeU64(values, i, diag.jitter_buffer.push_rejected_invalid);
    writeU64(values, i, diag.jitter_buffer.push_rejected_sanity);
    writeU64(values, i, diag.jitter_buffer.pull_calls);
    writeU64(values, i, diag.jitter_buffer.pull_frames);
    writeU64(values, i, diag.jitter_buffer.pull_silence_frames);
    writeU64(values, i, diag.jitter_buffer.fill_episodes);
    writeU64(values, i, diag.jitter_buffer.fill_corrected_slots);
    writeU64(values, i, diag.jitter_buffer.drop_episodes);
    writeU64(values, i, diag.jitter_buffer.drop_skipped_slots);

    // jitter_buffer gauge（当前态，与累计 counter 互补）
    writeI32(values, i, static_cast<std::int32_t>(diag.jitter_buffer.lead_slots));
    writeU64(values, i, diag.jitter_buffer.play_sequence);
    writeU64(values, i, diag.jitter_buffer.highest_received_sequence);
    writeU64(values, i, diag.jitter_buffer.consecutive_silence_frames);
    writeU64(values, i, diag.jitter_buffer.max_silence_run_frames);
    writeI32(values, i, diag.jitter_buffer.episode_state);
    writeI32(values, i, diag.jitter_buffer.reanchor_pending ? 1 : 0);
    writeU64(values, i, diag.jitter_buffer.reanchor_target_sequence);

    // playback 分组（声明顺序）
    writeU64(values, i, diag.playback.pull_calls);
    writeU64(values, i, diag.playback.pull_frames);
    writeU64(values, i, diag.playback.pull_silence_frames);

    // stream 分组（声明顺序；输出流实际运行参数）
    writeI32(values, i, static_cast<std::int32_t>(diag.stream.backend));
    writeI32(values, i, static_cast<std::int32_t>(diag.stream.sample_rate));
    writeI32(values, i, static_cast<std::int32_t>(diag.stream.channels));
    writeI32(values, i, diag.stream.performance_mode);
    writeI32(values, i, static_cast<std::int32_t>(diag.stream.frames_per_burst));
    writeI32(values, i, static_cast<std::int32_t>(diag.stream.buffer_capacity_frames));
    // stream 运行期统计（Gauge）
    writeU64(values, i, diag.stream.callback_count);
    writeI32(values, i, static_cast<std::int32_t>(diag.stream.current_padding_frames));
    writeU64(values, i, diag.stream.xrun_count);

    // Phase 0 网络观测（0.3.0 末尾追加，与 C 结构体顺序一致；double 位模式）。
    writeF64(values, i, diag.estimator_jitter_ms);
    writeF64(values, i, diag.estimator_base_delay_ms);
    writeF64(values, i, diag.estimator_transit_ms);
    writeU64(values, i, diag.estimator_reordered_packets);
    writeU64(values, i, diag.estimator_duplicate_packets);
    writeU64(values, i, diag.estimator_late_packets);

    // Phase 1 自适应 target（末尾追加，与 C 结构体顺序一致）。
    writeI32(values, i, static_cast<std::int32_t>(diag.target_slots));
    writeF64(values, i, diag.target_ms);

    // Phase 2 欠载预算 + PCM concealment（末尾追加）。
    writeU64(values, i, diag.underrun_events);
    writeU64(values, i, diag.underrun_frames);
    writeU64(values, i, diag.max_consecutive_underrun_slots);
    writeU64(values, i, diag.concealed_slots);
    writeU64(values, i, diag.concealed_saturated_slots);
    writeU64(values, i, diag.late_useful_packets);
    writeF64(values, i, diag.underrun_ratio);
    writeF64(values, i, diag.fill_duty);
    writeF64(values, i, diag.drop_duty);
    // 细则 §11：lead_ms 与 target/jitter 同快照（末尾追加）。
    writeF64(values, i, diag.lead_ms);

    // Buffer 决策层观测（末尾追加，与 aqua_jitter_control_stats_t 声明顺序一致）。
    // 决策层 11 项
    writeI32(values, i, diag.jitter_control.adaptive);
    writeI32(values, i, static_cast<std::int32_t>(diag.jitter_control.desired_slots));
    writeI32(values, i, static_cast<std::int32_t>(diag.jitter_control.min_slots));
    writeI32(values, i, static_cast<std::int32_t>(diag.jitter_control.max_slots));
    writeI32(values, i, diag.jitter_control.margin_source);
    writeI32(values, i, diag.jitter_control.path);
    writeI32(values, i, diag.jitter_control.floor_bound);
    writeI32(values, i, diag.jitter_control.cap_bound);
    writeF64(values, i, diag.jitter_control.underrun_penalty);
    writeF64(values, i, diag.jitter_control.dwell_remaining_ms);
    writeF64(values, i, diag.jitter_control.fall_room_slots);
    // 观测层尾部 4 项
    writeU64(values, i, diag.jitter_control.stall_events);
    writeF64(values, i, diag.jitter_control.stall_peak_ms);
    writeF64(values, i, diag.jitter_control.last_stall_gap_ms);
    writeF64(values, i, diag.jitter_control.arrival_interval_ms);
    // 执行层 6 项
    writeI32(values, i, static_cast<std::int32_t>(diag.jitter_control.band_warning_low));
    writeI32(values, i, static_cast<std::int32_t>(diag.jitter_control.band_normal_low));
    writeI32(values, i, static_cast<std::int32_t>(diag.jitter_control.band_normal_high));
    writeI32(values, i, static_cast<std::int32_t>(diag.jitter_control.band_warning_high));
    writeI32(values, i, static_cast<std::int32_t>(diag.jitter_control.conceal_run_slots));
    writeI32(values, i, static_cast<std::int32_t>(diag.jitter_control.underrun_run_slots));

    // 切换事务序号（末尾追加）：UI 判定"又切了一次"的可靠判据（见 aqua_capi.h）。
    writeI32(values, i, static_cast<std::int32_t>(diag.switch_seq));

    if (i != kDiagnosticsCount) {
        __android_log_print(ANDROID_LOG_ERROR, kTagAqua,
            "jni diagnostics field count mismatch: wrote %d of %d",
            static_cast<int>(i), static_cast<int>(kDiagnosticsCount));
    }
    jlongArray array = env->NewLongArray(static_cast<jsize>(kDiagnosticsCount));
    if (array == nullptr) {
        return nullptr; // OOM 已抛出
    }
    env->SetLongArrayRegion(array, 0, static_cast<jsize>(kDiagnosticsCount), values.data());
    return array;
}

jintArray nativeGetConnectResult(JNIEnv* env, jobject, jlong handle)
{
    auto* client = as_client(handle);
    if (client == nullptr) {
        return nullptr;
    }

    aqua_connect_result_t result { };
    if (aqua_client_get_connect_result(client, &result) != AQUA_OK) {
        return nullptr; // 未连接
    }

    constexpr jsize kConnectResultCount = 7;
    const jint values[kConnectResultCount] = {
        static_cast<jint>(result.session_id),
        static_cast<jint>(result.advertised_udp_port),
        result.audio_encoding,
        static_cast<jint>(result.channels),
        static_cast<jint>(result.sample_rate),
        static_cast<jint>(result.frame_count),
        static_cast<jint>(result.learned_udp_port),
    };
    jintArray array = env->NewIntArray(kConnectResultCount);
    if (array == nullptr) {
        return nullptr;
    }
    env->SetIntArrayRegion(array, 0, kConnectResultCount, values);
    return array;
}

jstring nativeGetAdvertisedUdpAddress(JNIEnv* env, jobject, jlong handle)
{
    auto* client = as_client(handle);
    if (client == nullptr) {
        return nullptr;
    }
    aqua_connect_result_t result { };
    if (aqua_client_get_connect_result(client, &result) != AQUA_OK) {
        return nullptr;
    }
    return env->NewStringUTF(result.advertised_udp_address);
}

jstring nativeGetLearnedUdpAddress(JNIEnv* env, jobject, jlong handle)
{
    auto* client = as_client(handle);
    if (client == nullptr) {
        return nullptr;
    }
    aqua_connect_result_t result { };
    if (aqua_client_get_connect_result(client, &result) != AQUA_OK) {
        return nullptr;
    }
    if (result.learned_udp_address[0] == '\0') {
        return nullptr; // 尚未学到
    }
    return env->NewStringUTF(result.learned_udp_address);
}

jstring nativeGetVersion(JNIEnv* env, jobject)
{
    return env->NewStringUTF(aqua_version());
}

// 设备切换（playback_switching_design.md §9）：deviceId == -1 = 跟随系统；
// 否则编码为 "android:N"（AAudio setDeviceId 的 native 词汇）。
jint nativeSetPlaybackDevice(JNIEnv*, jobject, jlong handle, jint device_id)
{
    auto* client = as_client(handle);
    if (client == nullptr) {
        return AQUA_ERR_INVALID_ARGUMENT;
    }
    if (device_id < 0) {
        return aqua_client_set_playback_device(client, nullptr);
    }
    char encoded[32];
    std::snprintf(encoded, sizeof(encoded), "android:%d", static_cast<int>(device_id));
    return aqua_client_set_playback_device(client, encoded);
}

// 设备集合变化推送（playback_switching_design.md §5 rev2）：当前可选输出
// 设备 id 全集（AudioDeviceInfo.id），JNI 编码 "android:N"（与
// nativeSetPlaybackDevice 同一词汇，Kotlin 不做字符串拼接）。core 内部
// 1s 合并去抖后完成全部路由决策；Kotlin 只转发快照。
void nativeNotifyDevicesChanged(JNIEnv* env, jobject, jlong handle, jintArray ids)
{
    auto* client = as_client(handle);
    if (client == nullptr) {
        return;
    }
    const jsize count = ids != nullptr ? env->GetArrayLength(ids) : 0;
    std::vector<std::string> encoded;
    std::vector<const char*> ptrs;
    if (count > 0) {
        encoded.reserve(static_cast<std::size_t>(count));
        ptrs.reserve(static_cast<std::size_t>(count));
        std::vector<jint> values(static_cast<std::size_t>(count));
        env->GetIntArrayRegion(ids, 0, count, values.data());
        for (const jint id : values) {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "android:%d", static_cast<int>(id));
            encoded.emplace_back(buf);
        }
        for (const auto& s : encoded) {
            ptrs.push_back(s.c_str());
        }
    }
    aqua_client_notify_devices_changed(client, ptrs.data(),
        static_cast<std::int32_t>(ptrs.size()));
}

// 设备 id 字符串查询：Array(2) = [requested, stream]；空串 = 无 / 未知。
jobjectArray nativeGetPlaybackDeviceIds(JNIEnv* env, jobject, jlong handle)
{
    auto* client = as_client(handle);
    if (client == nullptr) {
        return nullptr;
    }
    aqua_client_diagnostics_t diag { };
    if (aqua_client_get_diagnostics(client, &diag) != AQUA_OK) {
        return nullptr;
    }
    const jclass string_cls = env->FindClass("java/lang/String");
    if (string_cls == nullptr) {
        return nullptr;
    }
    jobjectArray array = env->NewObjectArray(2, string_cls, nullptr);
    if (array == nullptr) {
        return nullptr; // OOM 已抛出
    }
    env->SetObjectArrayElement(array, 0, env->NewStringUTF(diag.requested_device_id));
    env->SetObjectArrayElement(array, 1, env->NewStringUTF(diag.stream_device_id));
    return array;
}

const JNINativeMethod kMethods[] = {
    { "nativeCreate",
        "(Ljava/lang/String;ILjava/lang/String;IIIIIZZIDIDDDD)J",
        reinterpret_cast<void*>(&nativeCreate) },
    { "nativeStart", "(J)I", reinterpret_cast<void*>(&nativeStart) },
    { "nativeStop", "(J)I", reinterpret_cast<void*>(&nativeStop) },
    { "nativeDestroy", "(J)V", reinterpret_cast<void*>(&nativeDestroy) },
    { "nativeGetState", "(J)I", reinterpret_cast<void*>(&nativeGetState) },
    { "nativeGetLastAudioError", "(J)I",
        reinterpret_cast<void*>(&nativeGetLastAudioError) },
    { "nativeGetAudioErrorEpoch", "(J)J",
        reinterpret_cast<void*>(&nativeGetAudioErrorEpoch) },
    { "nativeGetLastErrorName", "(J)Ljava/lang/String;",
        reinterpret_cast<void*>(&nativeGetLastErrorName) },
    { "nativeGetDiagnostics", "(J)[J",
        reinterpret_cast<void*>(&nativeGetDiagnostics) },
    { "nativeGetConnectResult", "(J)[I",
        reinterpret_cast<void*>(&nativeGetConnectResult) },
    { "nativeGetAdvertisedUdpAddress", "(J)Ljava/lang/String;",
        reinterpret_cast<void*>(&nativeGetAdvertisedUdpAddress) },
    { "nativeGetLearnedUdpAddress", "(J)Ljava/lang/String;",
        reinterpret_cast<void*>(&nativeGetLearnedUdpAddress) },
    { "nativeGetVersion", "()Ljava/lang/String;",
        reinterpret_cast<void*>(&nativeGetVersion) },
    { "nativeSetPlaybackDevice", "(JI)I",
        reinterpret_cast<void*>(&nativeSetPlaybackDevice) },
    { "nativeNotifyDevicesChanged", "(J[I)V",
        reinterpret_cast<void*>(&nativeNotifyDevicesChanged) },
    { "nativeGetPlaybackDeviceIds", "(J)[Ljava/lang/String;",
        reinterpret_cast<void*>(&nativeGetPlaybackDeviceIds) },
};

} // namespace

// System.loadLibrary("aqua") 触发的 JNI_OnLoad：动态注册全部方法。
jint JNI_OnLoad(JavaVM* vm, void* reserved)
{
    (void)reserved;
    JNIEnv* env = nullptr;
    if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK
        || env == nullptr) {
        return JNI_ERR;
    }

    const jclass cls = env->FindClass("com/aquawius/aqua/native/AquaNative");
    if (cls == nullptr) {
        __android_log_print(ANDROID_LOG_ERROR, kTagAqua,
            "jni: AquaNative class not found");
        return JNI_ERR;
    }

    constexpr jint kMethodCount = static_cast<jint>(std::size(kMethods));
    if (env->RegisterNatives(cls, kMethods, kMethodCount) != JNI_OK) {
        __android_log_print(ANDROID_LOG_ERROR, kTagAqua,
            "jni: RegisterNatives failed");
        return JNI_ERR;
    }

    __android_log_print(ANDROID_LOG_INFO, kTagAqua,
        "jni: AquaNative registered (%d methods)", static_cast<int>(kMethodCount));
    return JNI_VERSION_1_6;
}
