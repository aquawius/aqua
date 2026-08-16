package com.aquawius.aqua.native

/**
 * JNI 薄桥：与 src/android/jni/aqua_jni.cpp 动态注册的 native 方法一一对应。
 *
 * M1 只暴露 C API 的最小轮询面（无 native -> Kotlin 回调）：
 * create / start / shutdown / destroy / state / isRunning / lastError / diagnostics。
 */
object AquaNative {
    init {
        System.loadLibrary("aqua")
    }

    external fun nativeCreate(): Long

    external fun nativeDestroy(handle: Long)

    external fun nativeStart(
        handle: Long,
        serverIp: String,
        rpcPort: Int,
        jitterBufferMs: Int,
        playbackBufferSize: Long,
        autoReconnect: Boolean,
        clientName: String,
    ): Int

    external fun nativeShutdown(handle: Long): Int

    external fun nativeGetState(handle: Long): Int

    external fun nativeIsRunning(handle: Long): Boolean

    external fun nativeGetLastError(handle: Long): String

    /** 返回 double[27]，顺序见 AquaDiagnostics.fromArray；无快照时返回 null。 */
    external fun nativeGetDiagnostics(handle: Long): DoubleArray?

    /** 返回 int[3]{encoding, channels, sampleRate}；尚未连接成功时返回 null。 */
    external fun nativeGetAudioFormat(handle: Long): IntArray?

    /** 库版本字符串（aqua_version()）。 */
    external fun nativeGetVersion(): String
}
