// Aqua Android JNI 桥：动态注册，映射 com.aquawius.aqua.native.AquaNative。
//
// 契约与 AquaNative.kt 文档一致：
// - diagnostics: LongArray(58)，字段顺序 = aqua_client_diagnostics_t 扁平化
//   （state, last_audio_error, playback_running, playback_state, route_mode,
//   switch_outcome, switch_error 先，net/jb/playback/stream 分组随后，
//   每组内按结构体声明顺序）；uint64 -> Long（值直传，非位重解释）。
// - connectResult: IntArray(7) {sessionId, advertisedUdpPort, encoding, channels,
//   sampleRate, frameCount, learnedUdpPort}；未连接时返回 null；
// - nativeCreate 末两参数 playbackLowLatency / playbackPreferCurrent：
//   false = NONE + SHARED / FollowSystem，true = LOW_LATENCY + SHARED / PreferCurrent。
//   advertisedUdpAddress / learnedUdpAddress 单独查询（String）。
// - 设备路由（playback_switching_design.md §9）：
//   nativeSetPlaybackDevice(handle, int deviceId)：-1 = 跟随系统；否则编码为
//   "android:N"（Kotlin 无字符串拼接）；设备 id 字符串经
//   nativeGetPlaybackDeviceIds 查询（Array(2)：[requested, stream]，空串 = 无）。
//
// 线程模型：与 C API 一致——create/start/stop/destroy 由控制线程串行；
// 查询可任意线程轮询（250ms Compose 轮询 + 500ms Service 循环）。

#include <jni.h>

#include "aqua/c_api/aqua_capi.h"
#include "aqua/net/address/address_utils.h"

#include <android/log.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

constexpr char kTagAqua[] = "aqua";

void writeLong(JNIEnv* env, jlongArray array, jsize index, jlong value)
{
    env->SetLongArrayRegion(array, index, 1, &value);
}

void writeU64(JNIEnv* env, jlongArray array, jsize index, std::uint64_t value)
{
    // uint64 计数器 ≤ 2^63-1 的量级（时间×速率远不可达），值直传。
    writeLong(env, array, index, static_cast<jlong>(value));
}

void writeI64(JNIEnv* env, jlongArray array, jsize index, std::int64_t value)
{
    writeLong(env, array, index, static_cast<jlong>(value));
}

void writeI32(JNIEnv* env, jlongArray array, jsize index, std::int32_t value)
{
    writeLong(env, array, index, static_cast<jlong>(value));
}

void writeF64(JNIEnv* env, jlongArray array, jsize index, double value)
{
    static_assert(sizeof(double) == sizeof(jlong), "double/jlong size mismatch");
    jlong wide = 0;
    std::memcpy(&wide, &value, sizeof(double));
    writeLong(env, array, index, wide);
}

// ---- 动态注册表 ----

jlong nativeCreate(JNIEnv* env, jobject, jstring server_ip, jint rpc_port,
    jstring client_name, jint jitter_slots, jint hello_interval_ms,
    jint playback_frames, jint force_udp_port, jint log_level,
    jboolean playback_low_latency, jboolean playback_prefer_current)
{
    if (server_ip == nullptr) {
        return 0;
    }
    const char* server_ip_utf = env->GetStringUTFChars(server_ip, nullptr);
    if (server_ip_utf == nullptr) {
        return 0; // OOM 已抛出
    }

    const char* client_name_utf = nullptr;
    if (client_name != nullptr) {
        client_name_utf = env->GetStringUTFChars(client_name, nullptr);
        if (client_name_utf == nullptr) {
            env->ReleaseStringUTFChars(server_ip, server_ip_utf);
            return 0;
        }
    }

    aqua_client_config_t config { };
    config.server_ip = server_ip_utf;
    config.rpc_port = static_cast<std::uint16_t>(rpc_port);
    config.client_name = client_name_utf;
    config.jitter_buffer_slots = static_cast<std::uint32_t>(jitter_slots);
    config.hello_interval_ms = static_cast<std::uint32_t>(hello_interval_ms);
    config.playback_frames_per_buffer = static_cast<std::uint32_t>(playback_frames);
    config.force_udp_port = static_cast<std::uint16_t>(force_udp_port);
    config.log_level = log_level; // -1 = 保持进程当前级别
    config.playback_low_latency = playback_low_latency == JNI_TRUE ? 1 : 0;
    config.playback_prefer_current = playback_prefer_current == JNI_TRUE ? 1 : 0;

    aqua_client_t* client = aqua_client_create(&config);

    if (client_name_utf != nullptr) {
        env->ReleaseStringUTFChars(client_name, client_name_utf);
    }
    env->ReleaseStringUTFChars(server_ip, server_ip_utf);
    return reinterpret_cast<jlong>(client);
}

jint nativeStart(JNIEnv*, jobject, jlong handle)
{
    return aqua_client_start(reinterpret_cast<aqua_client_t*>(handle));
}

jint nativeStop(JNIEnv*, jobject, jlong handle)
{
    return aqua_client_stop(reinterpret_cast<aqua_client_t*>(handle));
}

void nativeDestroy(JNIEnv*, jobject, jlong handle)
{
    aqua_client_destroy(reinterpret_cast<aqua_client_t*>(handle));
}

jint nativeGetState(JNIEnv*, jobject, jlong handle)
{
    return aqua_client_get_state(reinterpret_cast<aqua_client_t*>(handle));
}

jint nativeGetLastAudioError(JNIEnv*, jobject, jlong handle)
{
    return aqua_client_get_last_audio_error(
        reinterpret_cast<aqua_client_t*>(handle));
}

jstring nativeGetLastErrorName(JNIEnv* env, jobject, jlong handle)
{
    const int error = aqua_client_get_last_audio_error(
        reinterpret_cast<aqua_client_t*>(handle));
    return env->NewStringUTF(aqua_audio_error_name(error));
}

// ---- diagnostics: LongArray(58) ----
// 顺序契约（与 aqua_client_diagnostics_t 声明顺序一一对应）：
// [0] state, [1] last_audio_error, [2] playback_running, [3] playback_state,
// [4] route_mode, [5] switch_outcome, [6] switch_error
// [7..25] net 分组 19 项（transport 9 + hello 4 + 分类 6）
// [26..45] jitter_buffer 分组 20 项
// [46..48] playback 分组 3 项
// [49..57] stream 分组 6 项（输出流实际运行参数）
jlongArray nativeGetDiagnostics(JNIEnv* env, jobject, jlong handle)
{
    auto* client = reinterpret_cast<aqua_client_t*>(handle);
    if (client == nullptr) {
        return nullptr;
    }

    aqua_client_diagnostics_t diag { };
    if (aqua_client_get_diagnostics(client, &diag) != AQUA_OK) {
        return nullptr;
    }

    constexpr jsize kDiagnosticsCount = 58;
    jlongArray array = env->NewLongArray(kDiagnosticsCount);
    if (array == nullptr) {
        return nullptr; // OOM 已抛出
    }

    jsize i = 0;
    writeI32(env, array, i++, diag.state);
    writeI32(env, array, i++, diag.last_audio_error);
    writeI32(env, array, i++, diag.playback_running);
    writeI32(env, array, i++, diag.playback_state);
    writeI32(env, array, i++, diag.route_mode);
    writeI32(env, array, i++, diag.switch_outcome);
    writeI32(env, array, i++, diag.switch_error);

    // net 分组（声明顺序）
    writeU64(env, array, i++, diag.net.rx_packets);
    writeU64(env, array, i++, diag.net.rx_bytes);
    writeU64(env, array, i++, diag.net.rx_errors);
    writeU64(env, array, i++, diag.net.tx_packets);
    writeU64(env, array, i++, diag.net.tx_bytes);
    writeU64(env, array, i++, diag.net.tx_errors);
    writeU64(env, array, i++, diag.net.tx_dropped);
    writeU64(env, array, i++, diag.net.tx_enqueue_failures);
    writeU64(env, array, i++, diag.net.tx_queue_depth);
    writeU64(env, array, i++, diag.net.hello_ack_count);
    writeI32(env, array, i++, static_cast<std::int32_t>(diag.net.hello_ack_misses));
    writeI64(env, array, i++, diag.net.hello_ack_age_ms);
    writeU64(env, array, i++, diag.net.hello_send_attempts);
    writeU64(env, array, i++, diag.net.hello_ack_miss_events);
    writeU64(env, array, i++, diag.net.audio_frames_accepted);
    writeU64(env, array, i++, diag.net.malformed_datagrams);
    writeU64(env, array, i++, diag.net.unexpected_sender_datagrams);
    writeU64(env, array, i++, diag.net.wrong_session_acks);
    writeU64(env, array, i++, diag.net.audio_payload_mismatches);
    writeU64(env, array, i++, diag.net.non_audio_datagrams);
    writeI32(env, array, i++, diag.net.hello_failed);

    // jitter_buffer 分组（声明顺序）
    writeF64(env, array, i++, diag.jitter_buffer.water_level);
    writeI32(env, array, i++, diag.jitter_buffer.used_slots);
    writeI32(env, array, i++, diag.jitter_buffer.capacity_slots);
    writeU64(env, array, i++, diag.jitter_buffer.reanchor_count);
    writeU64(env, array, i++, diag.jitter_buffer.reanchor_requests);
    writeU64(env, array, i++, diag.jitter_buffer.reanchor_cancels);
    writeU64(env, array, i++, diag.jitter_buffer.reanchor_sanity_rejections);
    writeU64(env, array, i++, diag.jitter_buffer.last_reanchor_sequence);
    writeU64(env, array, i++, diag.jitter_buffer.push_accepted);
    writeU64(env, array, i++, diag.jitter_buffer.push_rejected);
    writeU64(env, array, i++, diag.jitter_buffer.push_rejected_late);
    writeU64(env, array, i++, diag.jitter_buffer.push_rejected_slot_busy);
    writeU64(env, array, i++, diag.jitter_buffer.push_rejected_invalid);
    writeU64(env, array, i++, diag.jitter_buffer.push_rejected_sanity);
    writeU64(env, array, i++, diag.jitter_buffer.pull_calls);
    writeU64(env, array, i++, diag.jitter_buffer.pull_frames);
    writeU64(env, array, i++, diag.jitter_buffer.pull_silence_frames);
    writeU64(env, array, i++, diag.jitter_buffer.fill_episodes);
    writeU64(env, array, i++, diag.jitter_buffer.fill_corrected_slots);
    writeU64(env, array, i++, diag.jitter_buffer.drop_episodes);
    writeU64(env, array, i++, diag.jitter_buffer.drop_skipped_slots);

    // playback 分组（声明顺序）
    writeU64(env, array, i++, diag.playback.pull_calls);
    writeU64(env, array, i++, diag.playback.pull_frames);
    writeU64(env, array, i++, diag.playback.pull_silence_frames);

    // stream 分组（声明顺序；输出流实际运行参数）
    writeI32(env, array, i++, static_cast<std::int32_t>(diag.stream.backend));
    writeI32(env, array, i++, static_cast<std::int32_t>(diag.stream.sample_rate));
    writeI32(env, array, i++, static_cast<std::int32_t>(diag.stream.channels));
    writeI32(env, array, i++, diag.stream.performance_mode);
    writeI32(env, array, i++, static_cast<std::int32_t>(diag.stream.frames_per_burst));
    writeI32(env, array, i++, static_cast<std::int32_t>(diag.stream.buffer_capacity_frames));

    if (i != kDiagnosticsCount) {
        __android_log_print(ANDROID_LOG_ERROR, kTagAqua,
            "jni diagnostics field count mismatch: wrote %d of %d",
            static_cast<int>(i), static_cast<int>(kDiagnosticsCount));
    }
    return array;
}

jintArray nativeGetConnectResult(JNIEnv* env, jobject, jlong handle)
{
    auto* client = reinterpret_cast<aqua_client_t*>(handle);
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
    auto* client = reinterpret_cast<aqua_client_t*>(handle);
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
    auto* client = reinterpret_cast<aqua_client_t*>(handle);
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
    auto* client = reinterpret_cast<aqua_client_t*>(handle);
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

// 设备 id 字符串查询：Array(2) = [requested, stream]；空串 = 无 / 未知。
jobjectArray nativeGetPlaybackDeviceIds(JNIEnv* env, jobject, jlong handle)
{
    auto* client = reinterpret_cast<aqua_client_t*>(handle);
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
        "(Ljava/lang/String;ILjava/lang/String;IIIIIZZ)J",
        reinterpret_cast<void*>(&nativeCreate) },
    { "nativeStart", "(J)I", reinterpret_cast<void*>(&nativeStart) },
    { "nativeStop", "(J)I", reinterpret_cast<void*>(&nativeStop) },
    { "nativeDestroy", "(J)V", reinterpret_cast<void*>(&nativeDestroy) },
    { "nativeGetState", "(J)I", reinterpret_cast<void*>(&nativeGetState) },
    { "nativeGetLastAudioError", "(J)I",
        reinterpret_cast<void*>(&nativeGetLastAudioError) },
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

    constexpr jint kMethodCount = static_cast<jint>(sizeof(kMethods) / sizeof(kMethods[0]));
    if (env->RegisterNatives(cls, kMethods, kMethodCount) != JNI_OK) {
        __android_log_print(ANDROID_LOG_ERROR, kTagAqua,
            "jni: RegisterNatives failed");
        return JNI_ERR;
    }

    __android_log_print(ANDROID_LOG_INFO, kTagAqua,
        "jni: AquaNative registered (%d methods)", static_cast<int>(kMethodCount));
    return JNI_VERSION_1_6;
}
