package com.aquawius.aqua

import com.aquawius.aqua.native.AquaNative

/**
 * 客户端运行状态，对应 C 侧 RuntimeState（aqua_runtime_state 枚举镜像）。
 * 数值与 core 枚举声明顺序一一对应，禁止改动。
 */
enum class AquaRuntimeState(val code: Int, val label: String) {
    CREATED(0, "未连接"),
    STARTING(1, "连接中"),
    RUNNING(2, "播放中"),
    DEGRADED(3, "已降级"),
    STOPPING(4, "停止中"),
    STOPPED(5, "已停止");

    companion object {
        fun fromCode(code: Int): AquaRuntimeState =
            entries.firstOrNull { it.code == code } ?: CREATED
    }
}

/** 运行期音频错误，对应 C 侧 AudioError 枚举镜像。 */
enum class AquaAudioError(val code: Int, val label: String) {
    NONE(0, "无"),
    DEVICE_NOT_FOUND(1, "设备不存在"),
    DEVICE_UNAVAILABLE(2, "设备不可用"),
    DEVICE_DISCONNECTED(3, "设备已断开"),
    FORMAT_UNSUPPORTED(4, "格式不支持"),
    NOT_SUPPORTED(5, "不支持的操作"),
    PERMISSION_DENIED(6, "权限被拒"),
    ALREADY_RUNNING(7, "已在运行"),
    NOT_RUNNING(8, "未在运行"),
    INVALID_ARGUMENT(9, "参数无效"),
    BACKEND_FAILED(10, "后端失败");

    companion object {
        fun fromCode(code: Int): AquaAudioError =
            entries.firstOrNull { it.code == code } ?: NONE
    }
}

/** 音频编码，对应 core AudioEncoding（与 proto 枚举同数值）。 */
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

/** host:port 格式化：IPv6 字面量加方括号，IPv4 不加；已带方括号则原样。 */
fun formatHostPort(host: String, port: Int): String =
    if (host.contains(':') && !host.startsWith('[')) "[$host]:$port" else "$host:$port"

/** 连接结果（音频契约 + 数据面 endpoint），对应 C 侧 aqua_connect_result_t。 */
data class AquaConnectResult(
    val sessionId: Long,
    val advertisedUdpAddress: String,
    val advertisedUdpPort: Int,
    val encoding: AquaEncoding,
    val channels: Int,
    val sampleRate: Int,
    val frameCount: Int,
    /** 动态值：当前学到的实际对端（HELLO_ACK 来源），每次有效 HELLO_ACK 刷新；
     *  不是一次性初始化参数；空串 = 尚未学到。 */
    val learnedUdpAddress: String,
    val learnedUdpPort: Int,
) {
    /** 码率（kbps）：采样率 × 声道 × 位深 / 1000。 */
    val bitRateKbps: Int
        get() = if (encoding.bitsPerSample > 0) {
            sampleRate * channels * encoding.bitsPerSample / 1000
        } else {
            0
        }
}

/**
 * 回放客户端封装：薄包装 AquaNative，配置 + 生命周期 + 轮询查询。
 *
 * 轮询模型（无 native 回调）：调用方周期拉取 state()/diagnostics()/connectResult()。
 * 句柄不是线程安全的，create/start/stop/destroy 需串行调用（与 C API 契约一致）；
 * 查询可任意线程。
 *
 * 生命周期（一次性）：create → start（成功则 RUNNING/DEGRADED，失败则 STOPPED）
 * → stop → destroy。重连 = destroy 后重新 create（由 Controller 层驱动）。
 */
class AquaClient(
    val serverIp: String,
    val rpcPort: Int,
    val clientName: String,
    val jitterBufferSlots: Int = 0,       // 0 = core 默认 30 slots
    val helloIntervalMs: Int = 0,         // 0 = core 默认 1000ms
    val playbackFramesPerBuffer: Int = 0, // 0 = backend 自适应（设计决议）
    val forceUdpPort: Int = 0,            // 0 = 采用 server 通告
    val logLevel: Int = -1,               // -1 = 保持进程当前级别
    val playbackLowLatency: Boolean = true, // Android AAudio: true = LOW_LATENCY + SHARED, false = NONE + SHARED
) {
    @Volatile
    private var handle: Long = 0

    val isCreated: Boolean get() = handle != 0L

    /** 创建并启动（start 阻塞至 gRPC Connect 完成）。返回 0 = AQUA_OK；
     *  失败时 handle 处于 STOPPED 态，只能 destroy 重建。 */
    fun connect(): Int {
        if (handle != 0L) return STATUS_OK
        handle = AquaNative.nativeCreate(
            serverIp = serverIp,
            rpcPort = rpcPort,
            clientName = clientName,
            jitterBufferSlots = jitterBufferSlots,
            helloIntervalMs = helloIntervalMs,
            playbackFramesPerBuffer = playbackFramesPerBuffer,
            forceUdpPort = forceUdpPort,
            logLevel = logLevel,
            playbackLowLatency = playbackLowLatency,
        )
        if (handle == 0L) return STATUS_CREATE_FAILED
        return AquaNative.nativeStart(handle)
    }

    /** 停止（幂等）：停止 runtime、断开 gRPC、join 内部 IO 线程。 */
    fun stop() {
        if (handle != 0L) {
            AquaNative.nativeStop(handle)
        }
    }

    /** 销毁句柄（隐式 stop）。 */
    fun destroy() {
        if (handle != 0L) {
            AquaNative.nativeDestroy(handle)
            handle = 0
        }
    }

    fun state(): AquaRuntimeState =
        if (handle == 0L) AquaRuntimeState.CREATED
        else AquaRuntimeState.fromCode(AquaNative.nativeGetState(handle))

    /** 最近一次 audio 错误（设备断开/格式不支持等；NONE = 无）。 */
    fun lastAudioError(): AquaAudioError =
        if (handle == 0L) AquaAudioError.NONE
        else AquaAudioError.fromCode(AquaNative.nativeGetLastAudioError(handle))

    /** 最近一次 audio 错误名（C 侧静态字符串）。 */
    fun lastAudioErrorName(): String =
        if (handle == 0L) "" else AquaNative.nativeGetLastErrorName(handle)

    /** 诊断快照；handle 无效时返回 null。 */
    fun diagnostics(): AquaDiagnostics? =
        if (handle == 0L) null
        else AquaNative.nativeGetDiagnostics(handle)?.let { AquaDiagnostics.fromArray(it) }

    /** 当前会话连接结果（音频契约）；尚未连接成功时返回 null。 */
    fun connectResult(): AquaConnectResult? {
        if (handle == 0L) return null
        val a = AquaNative.nativeGetConnectResult(handle) ?: return null
        if (a.size != 7) return null
        val address = AquaNative.nativeGetAdvertisedUdpAddress(handle) ?: return null
        val learnedAddress = AquaNative.nativeGetLearnedUdpAddress(handle) ?: ""
        return AquaConnectResult(
            sessionId = a[0].toLong() and 0xFFFFFFFFL,
            advertisedUdpAddress = address,
            advertisedUdpPort = a[1],
            encoding = AquaEncoding.fromCode(a[2]),
            channels = a[3],
            sampleRate = a[4],
            frameCount = a[5],
            learnedUdpAddress = learnedAddress,
            learnedUdpPort = a[6],
        )
    }

    /** 库版本字符串（aqua_version()，全局，无需句柄）。 */
    fun version(): String = AquaNative.nativeGetVersion()

    companion object {
        const val STATUS_OK = 0
        const val STATUS_CREATE_FAILED = -1
        // C API 错误码（AQUA_ERR_*）
        const val ERR_INVALID_ARGUMENT = 1
        const val ERR_START_FAILED = 2
    }
}
