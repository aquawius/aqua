package com.aquawius.aqua.native

/**
 * JNI 薄桥：与 aqua_core C API (aqua_capi.h) 动态注册的 native 方法一一对应。
 *
 * M1 轮询模型（无 native -> Kotlin 回调）：Compose/Controller 周期拉取
 * state / lastError / diagnostics / connectResult。
 *
 * 契约（与 C API 头文件一致，字段顺序是 Kotlin 解码的固定契约）：
 * - nativeGetDiagnostics 返回 LongArray(48)，顺序见 AquaDiagnostics.fromArray；
 * - nativeGetConnectResult 返回 IntArray(7)：{sessionId, advertisedUdpPort, encoding,
 *   channels, sampleRate, frameCount, learnedUdpPort}，未连接时返回 null；
 *   advertisedUdpAddress / learnedUdpAddress 单独查询。
 */
object AquaNative {
    init {
        System.loadLibrary("aqua")
    }

    // ---- 生命周期 ----
    external fun nativeCreate(
        serverIp: String,
        rpcPort: Int,
        clientName: String,
        jitterBufferSlots: Int,
        helloIntervalMs: Int,
        playbackFramesPerBuffer: Int,
        forceUdpPort: Int,
        logLevel: Int,
    ): Long

    external fun nativeStart(handle: Long): Int

    external fun nativeStop(handle: Long): Int

    external fun nativeDestroy(handle: Long)

    // ---- 查询（轮询面）----
    external fun nativeGetState(handle: Long): Int

    external fun nativeGetLastAudioError(handle: Long): Int

    external fun nativeGetLastErrorName(handle: Long): String

    /** 诊断快照 LongArray(48)；handle 无效时返回 null。 */
    external fun nativeGetDiagnostics(handle: Long): LongArray?

    /** IntArray(7)：{sessionId, advertisedUdpPort, encoding, channels, sampleRate,
     *  frameCount, learnedUdpPort}；未连接时返回 null。 */
    external fun nativeGetConnectResult(handle: Long): IntArray?

    /** 服务端通告的 UDP 数据面地址（字面量）；未连接时返回 null。 */
    external fun nativeGetAdvertisedUdpAddress(handle: Long): String?

    /** 当前学到的 UDP peer 地址（HELLO_ACK 实际来源，字面量；动态值，每次有效 ACK 刷新）；
     *  未学到返回 null。 */
    external fun nativeGetLearnedUdpAddress(handle: Long): String?

    /** 库版本字符串（aqua_version()，全局，无需句柄）。 */
    external fun nativeGetVersion(): String
}
