package com.aquawius.aqua

import com.aquawius.aqua.native.AquaNative

/** 客户端状态，对应 C 侧 aqua_client_state_t。 */
enum class AquaClientState(val code: Int) {
    IDLE(0),
    CONNECTING(1),
    PLAYING(2),
    RECONNECTING(3),
    STOPPED(4),
    FAILED(5);

    companion object {
        fun fromCode(code: Int): AquaClientState =
            entries.firstOrNull { it.code == code } ?: IDLE
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
    var jitterLatencyMs: Int = 0,       // 0 = 默认 30ms
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

    companion object {
        const val STATUS_OK = 0
        const val STATUS_INVALID_ARGUMENT = -1
    }
}
