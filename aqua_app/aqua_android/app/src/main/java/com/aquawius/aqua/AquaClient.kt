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

/** 本地播放状态，对应 C 侧 PlaybackState（aqua_playback_state 枚举镜像）。
 *  数值与 core 枚举声明顺序一一对应，禁止改动。 */
enum class AquaPlaybackState(val code: Int, val label: String) {
    INACTIVE(0, "未激活"),
    STARTING(1, "启动中"),
    RUNNING(2, "运行中"),
    SWITCHING(3, "切换中"),
    FATAL(4, "致命错误");

    companion object {
        fun fromCode(code: Int): AquaPlaybackState =
            entries.firstOrNull { it.code == code } ?: INACTIVE
    }
}

/** 播放路由模式，对应 C 侧 PlaybackRouteMode（aqua_route_mode 枚举镜像）。
 *  路由是连接属性，不持久化；每次连接按"自动切换播放设备"设置起步。 */
enum class AquaRouteMode(val code: Int, val label: String) {
    FOLLOW_SYSTEM(0, "跟随系统"),
    PREFER_CURRENT(1, "优先当前设备"),
    PREFERRED_DEVICE(2, "指定设备");

    companion object {
        fun fromCode(code: Int): AquaRouteMode =
            entries.firstOrNull { it.code == code } ?: FOLLOW_SYSTEM
    }
}

/** 切换事务结果，对应 C 侧 SwitchOutcome（aqua_switch_outcome 枚举镜像）。 */
enum class AquaSwitchOutcome(val code: Int, val label: String) {
    NONE(0, "未切换"),
    SWITCHED(1, "已切换"),
    ROLLED_BACK(2, "已恢复原设备"),
    FELL_BACK_TO_SYSTEM(3, "已回退系统输出"),
    FATAL(4, "切换失败");

    companion object {
        fun fromCode(code: Int): AquaSwitchOutcome =
            entries.firstOrNull { it.code == code } ?: NONE
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
    /** 动态值：当前学到的实际对端（HeartbeatAck 来源），每次有效 HeartbeatAck 刷新；
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
 * JB 参数的网络环境预设：值取自 aqua_core/doc/jitter_buffer_control_design.md
 * §9.1「按网络环境的推荐起点」。
 *
 * **0 = 采用 core 默认值**：预设只写"需要偏离默认"的项，其余留 0——这样 core
 * 默认值随版本演进时，预设不会被冻结成一份过期的显式旧值。
 */
enum class JbPreset(
    val label: String,
    val summary: String,
    val jbCapacity: Int = 0,
    val jitterGain: Double = 0.0,
    val minTargetSlots: Int = 0,
    val stallPeakCapSlots: Double = 0.0,
    val stallPeakDecayMsPerSec: Double = 0.0,
    val stallThresholdPackets: Double = 0.0,
    val underrunPenaltySlots: Double = 0.0,
) {
    WIFI_LAN(
        label = "WiFi / LAN",
        summary = "同网段有线 / 常规 Wi-Fi：抖动与断流都很小，target 自然贴着几何地板" +
            "（约 15ms）。core 的默认值就是按这个场景定标的，所以这一档全部留 0。\n" +
            "这个档位下如果仍然卡顿，先别调 JB —— 去查网卡 / AP / 发送端，参数救不了。",
    ),
    WIFI_CROWDED(
        label = "Wi-Fi 拥挤",
        summary = "同链路常开下载或 AP 拥挤：在 Wi-Fi 组合之上把断流峰值上限抬到 10 槽" +
            "（37.5ms），让 30~34ms 那档（调度 / 漫游）也落进峰值项的线性区，" +
            "少让欠载反馈出面补课。代价是一次长断流会把延迟抬高得更久一点。",
        stallPeakCapSlots = 10.0,
    ),
    WAN(
        label = "公网",
        summary = "跨地域 / VPN / 4G-5G：先买够抖动吸收余量（容量 60 槽 ≈ 225ms），" +
            "把下限抬到 6 槽（≈22.5ms），峰值上限放到 16 槽（60ms）覆盖 50~60ms 的" +
            "常态间隙，衰减放慢到 5ms/s 让稀疏 stall 之间也记得住上次的教训。",
        jbCapacity = 60,
        minTargetSlots = 6,
        stallPeakCapSlots = 16.0,
        stallPeakDecayMsPerSec = 5.0,
    ),
    WEAK(
        label = "弱网",
        summary = "移动网络边缘 / 拥塞 AP：在公网组合之上把欠载惩罚步长翻倍到 2 槽——" +
            "散点丢包只把到达间隔拉到约 2 个包周期，远低于 stall 门，峰值项基本失明，" +
            "只能靠反馈闭环补偿；容量 80 槽吸收丢包与乱序。",
        jbCapacity = 80,
        minTargetSlots = 8,
        stallPeakCapSlots = 16.0,
        stallPeakDecayMsPerSec = 5.0,
        underrunPenaltySlots = 2.0,
    ),
    LOW_LATENCY(
        label = "低延迟优先",
        summary = "游戏语音 / 实时连麦：方向相反——容量压到 16 槽、断流峰值上限压到 4 槽" +
            "（约 15ms，只买最小保险），用抗抖动换延迟。\n" +
            "欠载率升到 0.5% 以内、靠掩盖兜住听感即算达标；超过说明这条链路配不上" +
            "这个延迟目标，退回「公网」档。",
        jbCapacity = 16,
        stallPeakCapSlots = 4.0,
    ),
    CUSTOM(
        label = "自定义",
        summary = "自己调：下面的滑块已解锁，改完下次连接生效。\n" +
            "顺序建议：先按网络挑一个预设，再看「当前状态」里的欠载率决定往哪边拧。",
    ),
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
    val jbCapacity: Int = 0,       // 0 = core 默认 30 slots
    val heartbeatHandshakeIntervalMs: Int = 0,         // 0 = core 默认 1000ms
    val playbackFramesPerBuffer: Int = 0, // 0 = backend 自适应（设计决议）
    val udpForcePort: Int = 0,            // 0 = 采用 server 通告
    val logLevel: Int = -1,               // -1 = 保持进程当前级别
    val playbackLowLatency: Boolean = true, // Android AAudio: true = LOW_LATENCY + SHARED, false = NONE + SHARED
    val playbackPreferCurrent: Boolean = false, // 路由起步：true = PreferCurrent（"自动切换"关）
    val initialPlaybackDeviceId: Int = -1, // 起步目标设备（首流初始化前选定）：-1 = 未指定
    // ---- JB 自适应调优（高级页；与 CLI --jb-* 同名项对齐）----
    // 全部沿用 C API 的 zero-init 惯例：**0.0 / 0 = 采用 core 默认值**。
    // 参数含义与调整方向见 aqua_core/doc/configuration_reference.md §5.1。
    val jitterGain: Double = 0.0, // k：margin = k×J（默认 5.0）；延迟↔稳定主力旋钮
    val minTargetSlots: Int = 0, // target 硬下限（默认 3 槽）；只能抬高，几何地板无条件托底
    val stallPeakCapSlots: Double = 0.0, // stall 峰值项上限（默认 8.0 槽 = 30ms）
    val stallPeakDecayMsPerSec: Double = 0.0, // stall 峰值衰减（默认 10.0 ms/s）；越小记得越久
    val stallThresholdPackets: Double = 0.0, // stall 门阈值（默认 5.0 个包周期）
    val underrunPenaltySlots: Double = 0.0, // 每次欠载抬升下限（默认 1.0 槽）
) {
    @Volatile
    private var handle: Long = 0

    val isCreated: Boolean get() = handle != 0L

    /** 句柄已创建后 start() 是否已成功过（重入判定的依据）。 */
    @Volatile
    private var started = false

    /** 创建并启动（start 阻塞至 gRPC Connect 完成）。返回 0 = AQUA_OK；
     *  失败时 handle 处于 STOPPED 态，只能 destroy 重建。
     *
     *  重入语义：已成功启动过 → 幂等返回 OK；句柄存在但上次 start 失败 →
     *  重试 start（旧实现只看 handle != 0 就返回 OK，会谎报成功）。 */
    fun connect(): Int {
        if (handle != 0L) {
            if (started) return STATUS_OK
            val rc = AquaNative.nativeStart(handle)
            started = rc == STATUS_OK
            return rc
        }
        handle = AquaNative.nativeCreate(
            serverIp = serverIp,
            rpcPort = rpcPort,
            clientName = clientName,
            jbCapacity = jbCapacity,
            heartbeatHandshakeIntervalMs = heartbeatHandshakeIntervalMs,
            playbackFramesPerBuffer = playbackFramesPerBuffer,
            udpForcePort = udpForcePort,
            logLevel = logLevel,
            playbackLowLatency = playbackLowLatency,
            playbackPreferCurrent = playbackPreferCurrent,
            initialDeviceId = initialPlaybackDeviceId,
            jitterGain = jitterGain,
            minTargetSlots = minTargetSlots,
            stallPeakCap = stallPeakCapSlots,
            stallPeakDecayMsPerSec = stallPeakDecayMsPerSec,
            stallThresholdPackets = stallThresholdPackets,
            underrunPenaltySlots = underrunPenaltySlots,
        )
        if (handle == 0L) return STATUS_CREATE_FAILED
        val rc = AquaNative.nativeStart(handle)
        started = rc == STATUS_OK
        return rc
    }

    /** 停止（幂等）：停止 runtime、断开 gRPC、join 内部 IO 线程。 */
    fun stop() {
        if (handle != 0L) {
            AquaNative.nativeStop(handle)
            started = false
        }
    }

    /** 销毁句柄（隐式 stop）。 */
    fun destroy() {
        if (handle != 0L) {
            AquaNative.nativeDestroy(handle)
            handle = 0
            started = false
        }
    }

    fun state(): AquaRuntimeState =
        if (handle == 0L) AquaRuntimeState.CREATED
        else AquaRuntimeState.fromCode(AquaNative.nativeGetState(handle))

    /** 当前 audio 错误（错误通道，非诊断快照字段）：NONE = 当前无未恢复
     *  错误（成功的恢复事务会清零，不再残留）。 */
    fun lastAudioError(): AquaAudioError =
        if (handle == 0L) AquaAudioError.NONE
        else AquaAudioError.fromCode(AquaNative.nativeGetLastAudioError(handle))

    /** 音频错误事件纪元：错误每次变化（置位新值 / 恢复清零）递增。
     *  轮询方以 epoch 变化检测错误事件与"已恢复"。 */
    fun audioErrorEpoch(): Long =
        if (handle == 0L) 0L else AquaNative.nativeGetAudioErrorEpoch(handle)

    /** 最近一次 audio 错误名（C 侧静态字符串）；JNI OOM 时退化为空串。 */
    fun lastAudioErrorName(): String =
        if (handle == 0L) "" else AquaNative.nativeGetLastErrorName(handle) ?: ""

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

    /** 库版本字符串（aqua_version()，全局，无需句柄）；JNI OOM 时退化为空串。 */
    fun version(): String = AquaNative.nativeGetVersion() ?: ""

    /** 显式切换播放设备：deviceId = -1 跟随系统；否则为 Android 音频设备 id
     *  （AudioDeviceInfo.id，JNI 编码为 "android:N"）。同步执行完整候选链，
     *  返回 0 = 事务完成（含降级成功，结果看诊断 switchOutcome）；
     *  3 = 未连接；4 = 切换链耗尽等终态拒绝。须与生命周期同线程串行调用（经 Controller 的 executor）。 */
    fun setPlaybackDevice(deviceId: Int): Int =
        if (handle == 0L) ERR_NOT_CONNECTED
        else AquaNative.nativeSetPlaybackDevice(handle, deviceId)

    /** 设备集合变化推送：当前可选输出设备 id 全集（AudioDeviceInfo.id）。
     *  core 内部合并去抖 + 全部路由决策；调用方只转发快照。 */
    fun notifyDevicesChanged(deviceIds: IntArray) {
        if (handle != 0L) {
            AquaNative.nativeNotifyDevicesChanged(handle, deviceIds)
        }
    }

    /** 设备 id 对（requested = 请求设备，stream = 实际输出回读；
     *  "android:N" 格式，空串 = 无 / 未知）。 */
    fun playbackDeviceIds(): Pair<String, String>? =
        if (handle == 0L) null
        else AquaNative.nativeGetPlaybackDeviceIds(handle)
            ?.takeIf { it.size == 2 }
            ?.let { it[0] to it[1] }

    companion object {
        const val STATUS_OK = 0
        const val STATUS_CREATE_FAILED = -1
        // C API 错误码（AQUA_ERR_*）
        const val ERR_INVALID_ARGUMENT = 1
        const val ERR_START_FAILED = 2
        const val ERR_NOT_CONNECTED = 3
        const val ERR_SWITCH_FAILED = 4
    }
}
