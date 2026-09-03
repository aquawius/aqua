package com.aquawius.aqua.native

/**
 * JNI 薄桥：与 aqua_core C API (aqua_capi.h) 动态注册的 native 方法一一对应。
 *
 * M1 轮询模型（无 native -> Kotlin 回调）：Compose/Controller 周期拉取
 * state / lastError / diagnostics / connectResult。
 *
 * 契约（与 C API 头文件一致，字段顺序是 Kotlin 解码的固定契约）：
 * - nativeGetDiagnostics 返回 LongArray(58)，顺序见 AquaDiagnostics.fromArray；
 * - nativeGetConnectResult 返回 IntArray(7)：{sessionId, advertisedUdpPort, encoding,
 *   channels, sampleRate, frameCount, learnedUdpPort}，未连接时返回 null；
 *   advertisedUdpAddress / learnedUdpAddress 单独查询。
 * - nativeCreate 末两参数 playbackLowLatency / playbackPreferCurrent：
 *   false = NONE + SHARED / FollowSystem，true = LOW_LATENCY + SHARED / PreferCurrent。
 * - 设备路由（playback_switching_design.md §9）：
 *   nativeSetPlaybackDevice(handle, int deviceId)：-1 = 跟随系统；否则由 JNI
 *   编码为 "android:N"（Kotlin 不做字符串拼接）。结果经诊断的
 *   routeMode / switchOutcome 观察。设备 id 字符串经
 *   nativeGetPlaybackDeviceIds 查询（Array(2)：[requested, stream]，空串 = 无）。
 */
object AquaNative {
    init {
        System.loadLibrary("aqua")
    }

    // ---- 生命周期 ----
    /** 创建 native client；playbackLowLatency 仅控制 Android/AAudio 的
     * performance mode，不启用 Exclusive；playbackPreferCurrent = "自动切换
     * 播放设备"关（PreferCurrent：首流成功后钉住实际设备）。
     * initialDeviceId = 起步目标播放设备（首流初始化前选定）：-1 = 未指定
     * （按 playbackPreferCurrent 起步）；否则首流直接在该设备打开，起步路由
     * = PreferredDevice（覆盖 playbackPreferCurrent），设备失效时首流回退
     * 系统默认（连接不因此失败，降级经诊断 routeMode 观察）。
     */
    external fun nativeCreate(
        serverIp: String,
        rpcPort: Int,
        clientName: String,
        jitterBufferSlots: Int,
        helloIntervalMs: Int,
        playbackFramesPerBuffer: Int,
        forceUdpPort: Int,
        logLevel: Int,
        playbackLowLatency: Boolean,
        playbackPreferCurrent: Boolean,
        initialDeviceId: Int,
    ): Long

    external fun nativeStart(handle: Long): Int

    external fun nativeStop(handle: Long): Int

    external fun nativeDestroy(handle: Long)

    // ---- 查询（轮询面）----
    external fun nativeGetState(handle: Long): Int

    external fun nativeGetLastAudioError(handle: Long): Int

    external fun nativeGetLastErrorName(handle: Long): String

    /** 诊断快照 LongArray(58)；handle 无效时返回 null。 */
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

    // ---- 播放设备切换（playback_switching_design.md §9）----
    /** 显式切换播放设备：deviceId = -1 跟随系统（FollowSystem）；否则编码为
     *  "android:N"（PreferredDevice）。同步执行完整候选链（target -> previous
     *  -> system_default），返回 0 = 事务完成（含降级成功，细节看诊断）。 */
    external fun nativeSetPlaybackDevice(handle: Long, deviceId: Int): Int

    /** 设备 id 字符串（Array(2) = [requested, stream]，"android:N" 格式；
     *  空串 = 无请求 / 未知）。 */
    external fun nativeGetPlaybackDeviceIds(handle: Long): Array<String>?
}
