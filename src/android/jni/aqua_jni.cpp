// Android JNI 薄桥：把 Kotlin external fun 映射到 aqua.h 的 C API。
//
// 设计约束（见 AGENT.md Android 方案）：
//   - JNI 只做参数/返回值的 JNI <-> C 转换，不承载任何业务逻辑（业务在 client_runtime）。
//   - M1 不实现 native -> Kotlin 回调：Kotlin 侧用协程轮询 state()/is_running()/
//     last_error()/get_diagnostics()。
//   - 使用动态注册（JNI_OnLoad + RegisterNatives），不导出 Java_com_* 静态符号，
//     与 C API 的 hidden visibility 导出策略一致。
//
// 目标 Java 类：com.aquawius.aqua.native.AquaNative

#include <jni.h>

#include "aqua.h"

namespace {

constexpr const char* kClassName = "com/aquawius/aqua/native/AquaNative";

// 诊断快照字段数，顺序与 aqua_diagnostics_t 一致（见 fill_diagnostics_array）。
constexpr jsize kDiagFieldCount = 27;

// ---- native 方法实现（M1：客户端回放端）----

jlong nativeCreate(JNIEnv* /*env*/, jobject /*self*/)
{
    return reinterpret_cast<jlong>(aqua_client_create());
}

void nativeDestroy(JNIEnv* /*env*/, jobject /*self*/, jlong handle)
{
    aqua_client_destroy(reinterpret_cast<aqua_client_t*>(handle));
}

jint nativeStart(JNIEnv* env, jobject /*self*/, jlong handle,
                 jstring server_ip, jint rpc_port,
                 jint jitter_buffer_ms,
                 jlong playback_buffer_size, jboolean auto_reconnect,
                 jstring client_name)
{
    if (handle == 0) {
        return AQUA_ERR_INVALID_ARGUMENT;
    }

    // 字符串入参在 start() 时被 aqua_client_start 拷贝，此处只需在调用期间保持有效。
    const char* server_ip_utf8 =
        server_ip != nullptr ? env->GetStringUTFChars(server_ip, nullptr) : nullptr;
    const char* client_name_utf8 =
        client_name != nullptr ? env->GetStringUTFChars(client_name, nullptr) : nullptr;

    aqua_client_config_t cfg;
    aqua_client_config_init(&cfg);
    cfg.server_ip = server_ip_utf8;
    cfg.server_rpc_port = static_cast<uint16_t>(rpc_port);
    cfg.jitter_buffer_ms = static_cast<uint32_t>(jitter_buffer_ms);
    cfg.playback_ringbuffer_size = static_cast<size_t>(playback_buffer_size);
    cfg.auto_reconnect = (auto_reconnect == JNI_TRUE) ? 1 : 0;
    cfg.client_name = client_name_utf8;

    // M1 不传回调（callbacks == nullptr）。
    const int rc =
        aqua_client_start(reinterpret_cast<aqua_client_t*>(handle), &cfg, nullptr);

    if (server_ip_utf8 != nullptr) {
        env->ReleaseStringUTFChars(server_ip, server_ip_utf8);
    }
    if (client_name_utf8 != nullptr) {
        env->ReleaseStringUTFChars(client_name, client_name_utf8);
    }
    return rc;
}

jint nativeShutdown(JNIEnv* /*env*/, jobject /*self*/, jlong handle)
{
    return aqua_client_shutdown(reinterpret_cast<aqua_client_t*>(handle));
}

jint nativeGetState(JNIEnv* /*env*/, jobject /*self*/, jlong handle)
{
    return static_cast<jint>(
        aqua_client_state(reinterpret_cast<const aqua_client_t*>(handle)));
}

jboolean nativeIsRunning(JNIEnv* /*env*/, jobject /*self*/, jlong handle)
{
    return aqua_client_is_running(reinterpret_cast<const aqua_client_t*>(handle)) != 0
        ? JNI_TRUE
        : JNI_FALSE;
}

jstring nativeGetLastError(JNIEnv* env, jobject /*self*/, jlong handle)
{
    return env->NewStringUTF(
        aqua_client_last_error(reinterpret_cast<const aqua_client_t*>(handle)));
}

// 诊断快照 -> double[27]（kDiagFieldCount），顺序与 aqua_diagnostics_t 字段一致。
// 用 double 承载全部字段（uint64/size_t 转 double；诊断计数远小于 2^53，无精度损失）。
void fill_diagnostics_array(JNIEnv* env, const aqua_diagnostics_t& d, jdoubleArray out)
{
    jdouble buf[kDiagFieldCount] = {
        d.rtt_ms,
        d.interarrival_jitter_ms,
        static_cast<jdouble>(d.packets_received),
        static_cast<jdouble>(d.packets_lost),
        static_cast<jdouble>(d.duplicates),
        static_cast<jdouble>(d.late_packets),
        static_cast<jdouble>(d.recv_audio_bytes),
        static_cast<jdouble>(d.recv_hello_acks),
        static_cast<jdouble>(d.jb_current_packets),
        d.jb_current_ms,
        d.jb_avg_ms,
        d.jb_min_ms,
        d.jb_max_ms,
        d.jb_capacity_ms,
        d.rb_current_ms,
        d.rb_avg_ms,
        d.rb_min_ms,
        d.rb_max_ms,
        d.rb_capacity_ms,
        static_cast<jdouble>(d.underruns),
        static_cast<jdouble>(d.deadline_misses),
        d.short_slope_samples_per_s,
        d.long_slope_samples_per_s,
        d.end_to_end_ms,
        d.drift_ppm,
        d.jb_target_ms,
        static_cast<jdouble>(d.rb_rearms),
    };
    env->SetDoubleArrayRegion(out, 0, kDiagFieldCount, buf);
}

jdoubleArray nativeGetDiagnostics(JNIEnv* env, jobject /*self*/, jlong handle)
{
    aqua_diagnostics_t d {};
    const int rc =
        aqua_client_get_diagnostics(reinterpret_cast<const aqua_client_t*>(handle), &d);
    if (rc != AQUA_OK) {
        return nullptr; // 尚无快照（未进入播放 / 首周期未到）
    }
    jdoubleArray out = env->NewDoubleArray(kDiagFieldCount);
    if (out == nullptr) {
        return nullptr;
    }
    fill_diagnostics_array(env, d, out);
    return out;
}

jstring nativeGetVersion(JNIEnv* env, jobject /*self*/)
{
    return env->NewStringUTF(aqua_version());
}

// 音频格式 -> int[3]{encoding, channels, sample_rate}；尚未拿到时返回 null。
jintArray nativeGetAudioFormat(JNIEnv* env, jobject /*self*/, jlong handle)
{
    aqua_audio_format_t f {};
    const int rc =
        aqua_client_get_audio_format(reinterpret_cast<const aqua_client_t*>(handle), &f);
    if (rc != AQUA_OK) {
        return nullptr;
    }
    constexpr jsize kFormatFieldCount = 3;
    jintArray out = env->NewIntArray(kFormatFieldCount);
    if (out == nullptr) {
        return nullptr;
    }
    jint buf[kFormatFieldCount] = {
        f.encoding,
        static_cast<jint>(f.channels),
        static_cast<jint>(f.sample_rate),
    };
    env->SetIntArrayRegion(out, 0, kFormatFieldCount, buf);
    return out;
}

const JNINativeMethod kMethods[] = {
    {"nativeCreate", "()J", reinterpret_cast<void*>(nativeCreate)},
    {"nativeDestroy", "(J)V", reinterpret_cast<void*>(nativeDestroy)},
    {"nativeStart",
     "(JLjava/lang/String;IIJZLjava/lang/String;)I",
     reinterpret_cast<void*>(nativeStart)},
    {"nativeShutdown", "(J)I", reinterpret_cast<void*>(nativeShutdown)},
    {"nativeGetState", "(J)I", reinterpret_cast<void*>(nativeGetState)},
    {"nativeIsRunning", "(J)Z", reinterpret_cast<void*>(nativeIsRunning)},
    {"nativeGetLastError", "(J)Ljava/lang/String;",
     reinterpret_cast<void*>(nativeGetLastError)},
    {"nativeGetDiagnostics", "(J)[D", reinterpret_cast<void*>(nativeGetDiagnostics)},
    {"nativeGetVersion", "()Ljava/lang/String;", reinterpret_cast<void*>(nativeGetVersion)},
    {"nativeGetAudioFormat", "(J)[I", reinterpret_cast<void*>(nativeGetAudioFormat)},
};

} // namespace

extern "C" JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void* /*reserved*/)
{
    JNIEnv* env = nullptr;
    if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) {
        return JNI_ERR;
    }

    jclass cls = env->FindClass(kClassName);
    if (cls == nullptr) {
        // 类名/包名不一致：让加载失败（比静默吞掉更利于排查）。
        return JNI_ERR;
    }

    if (env->RegisterNatives(cls, kMethods,
                             static_cast<jint>(sizeof(kMethods) / sizeof(kMethods[0])))
        != JNI_OK) {
        return JNI_ERR;
    }
    return JNI_VERSION_1_6;
}
