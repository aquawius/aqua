package com.aquawius.aqua

import com.aquawius.aqua.native.AquaNative

/** 客户端状态，对应 C 侧 aqua_client_state_t。 */
enum class AquaClientState(val code: Int, val label: String) {
    IDLE(0, "未连接"),
    CONNECTING(1, "连接中"),
    PLAYING(2, "播放中"),
    RECONNECTING(3, "重连中"),
    STOPPED(4, "已停止"),
    FAILED(5, "连接失败");

    companion object {
        fun fromCode(code: Int): AquaClientState =
            entries.firstOrNull { it.code == code } ?: IDLE
    }
}

/** 音频编码，对应 C 侧 aqua_encoding_t。 */
enum class AquaEncoding(val code: Int, val label: String, val bitsPerSample: Int) {
    INVALID(0, "未知", 0),
    PCM_S16LE(1, "PCM S16LE", 16),
    PCM_S32LE(2, "PCM S32LE", 32),
    PCM_F32LE(3, "PCM F32LE", 32),
    PCM_S24LE(4, "PCM S24LE", 24),
    PCM_U8(5, "PCM U8", 8);

    companion object {
        fun fromCode(code: Int): AquaEncoding =
            entries.firstOrNull { it.code == code } ?: INVALID
    }
}

/** 服务器音频格式，对应 C 侧 aqua_audio_format_t。 */
data class AquaAudioFormat(
    val encoding: AquaEncoding,
    val channels: Int,
    val sampleRate: Int,
) {
    /** 码率（kbps）：采样率 × 声道 × 位深 / 1000。 */
    val bitRateKbps: Int
        get() = if (encoding.bitsPerSample > 0) sampleRate * channels * encoding.bitsPerSample / 1000 else 0

    companion object {
        /** 从 JNI int[3]{encoding, channels, sampleRate} 解析。 */
        fun fromArray(a: IntArray): AquaAudioFormat? = if (a.size == 3) {
            AquaAudioFormat(
                encoding = AquaEncoding.fromCode(a[0]),
                channels = a[1],
                sampleRate = a[2],
            )
        } else {
            null
        }
    }
}

/**
 * 回放客户端封装：薄包装 AquaNative，提供配置 + 生命周期 + 轮询查询。
 *
 * M1 为轮询模型（无 native 回调）：调用方用协程周期性拉取 state()/diagnostics()。
 * 句柄不是线程安全的，create/start/shutdown/destroy 需串行调用（与 C API 契约一致）。
 */
class AquaClient(
    var serverIp: String = "127.0.0.1",
    var rpcPort: Int = 50051,
    var jitterLatencyMs: Int = 0,       // 0 = 默认 30ms（自适应 floor）
    var jitterMaxLatencyMs: Int = 0,    // 0 = 不启用显式 ceiling（上限 capacity/2）
    var driftThreshold: Int = 0,        // 0 = 默认 15
    var playbackBufferSize: Long = 0,   // 0 = 默认 16KB
    var autoReconnect: Boolean = false,
    var clientName: String = "aqua_android",
) {
    @Volatile
    private var handle: Long = 0

    val isCreated: Boolean get() = handle != 0L

    /** 创建句柄。幂等：已创建则忽略。 */
    fun create() {
        if (handle == 0L) {
            handle = AquaNative.nativeCreate()
        }
    }

    /** 启动（非阻塞）。返回 AQUA_OK(0) 表示已启动，其余为错误码。 */
    fun start(): Int {
        if (handle == 0L) return STATUS_INVALID_ARGUMENT
        return AquaNative.nativeStart(
            handle = handle,
            serverIp = serverIp,
            rpcPort = rpcPort,
            jitterLatencyMs = jitterLatencyMs,
            jitterMaxLatencyMs = jitterMaxLatencyMs,
            jitterAdaptWindowPackets = 0, // 暂不暴露到 UI：0 = core 默认 500 包
            driftThreshold = driftThreshold,
            playbackBufferSize = playbackBufferSize,
            autoReconnect = autoReconnect,
            clientName = clientName,
        )
    }

    /** 请求优雅关闭（非阻塞）。 */
    fun shutdown(): Int {
        if (handle == 0L) return STATUS_INVALID_ARGUMENT
        return AquaNative.nativeShutdown(handle)
    }

    /** 销毁句柄：内部先停止并 join，再释放。 */
    fun destroy() {
        if (handle != 0L) {
            AquaNative.nativeDestroy(handle)
            handle = 0
        }
    }

    fun state(): AquaClientState =
        AquaClientState.fromCode(if (handle == 0L) 0 else AquaNative.nativeGetState(handle))

    fun isRunning(): Boolean = handle != 0L && AquaNative.nativeIsRunning(handle)

    fun lastError(): String = if (handle == 0L) "" else AquaNative.nativeGetLastError(handle)

    /** 最近一次诊断快照；尚无快照时返回 null。 */
    fun diagnostics(): AquaDiagnostics? =
        if (handle == 0L) null
        else AquaNative.nativeGetDiagnostics(handle)?.let { AquaDiagnostics.fromArray(it) }

    /** 当前会话的服务器音频格式；尚未连接成功时返回 null。 */
    fun audioFormat(): AquaAudioFormat? =
        if (handle == 0L) null
        else AquaNative.nativeGetAudioFormat(handle)?.let { AquaAudioFormat.fromArray(it) }

    /** 库版本字符串（aqua_version()，全局，无需句柄）。 */
    fun version(): String = AquaNative.nativeGetVersion()

    companion object {
        const val STATUS_OK = 0
        const val STATUS_INVALID_ARGUMENT = -1
    }
}
