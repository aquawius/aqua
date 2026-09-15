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
 * JB 参数的延迟梯度预设：滑块从左到右 = 延迟从小到大 / 网络从优良到恶劣。
 * 取值参考 aqua_core/doc/jitter_buffer_control_design.md §9.1 的推荐起点，
 * 并按"低延迟模式"这一默认配置定标（非低延迟模式设备取数更大，需要更右侧的档）。
 *
 * **0 = 采用 core 默认值**：预设只写"需要偏离默认"的项，其余留 0——这样 core
 * 默认值随版本演进时，预设不会被冻结成一份过期的显式旧值。
 *
 * `hint` 是滑块下方的短说明（一句话），`summary` 是 tips 框里的完整说明。
 * 排序约束：滑块从左到右延迟递增，因此**容量（水位上限的 2/3）与水位下限（minTargetSlots）都必须单调不减** —— 两者任一出现回落，用户拖动滑块时就会遇到"往右拖反而更低延迟"的自相矛盾档位（实测 LAN/50ms/公网三档曾违反此约束）。
 */
enum class JbPreset(
    val label: String,
    val hint: String,
    val summary: String,
    val jbCapacity: Int = 0,
    val jitterGain: Double = 0.0,
    val minTargetSlots: Int = 0,
    val stallPeakCapSlots: Double = 0.0,
    val stallPeakDecayMsPerSec: Double = 0.0,
    val stallThresholdPackets: Double = 0.0,
    val underrunPenaltySlots: Double = 0.0,
) {
    ULTRA_LOW(
        label = "极低延迟",
        hint = "容量 16 槽，水位最多 10 槽（约 36ms）",
        summary = "容量 16 槽，自动水位上限随之降至 10 槽（约 36ms），断流应对上限 4 槽。\n" +
                "适用于局域网内的实时对讲与游戏语音。网络条件劣化时欠载概率上升，" +
                "此时应选择右侧相邻档位。",
        jbCapacity = 16,
        stallPeakCapSlots = 4.0,
    ),
    LAN_WIFI(
        label = "LAN / Wi-Fi",
        hint = "默认组合：容量 30 槽，水位最多 20 槽（约 73ms）",
        summary = "core 的默认值就是按常规有线 / Wi-Fi 定标的，所以这一档全部留 0。\n" +
                "网络干净时水位会自己压到地板（约 15ms），抖动大时最多到 20 槽。\n" +
                "该档位下若仍出现欠载，应优先排查 AP、网卡与发送端，而非继续调整参数。",
    ),
    WIFI_CROWDED(
        label = "Wi-Fi 拥挤",
        hint = "同档延迟，但断流应对上限抬到 10 槽（约 36ms）",
        summary = "延迟与上一档相同，断流应对上限提升至 10 槽（约 36ms）。\n" +
                "适用于持续下载、AP 繁忙或同频干扰较强的环境，可覆盖 30ms 量级的到达间隔突增。\n" +
                "断流结束后延迟回落所需时间相应延长。",
        stallPeakCapSlots = 10.0,
    ),
    WIFI_SMOOTH(
        label = "Wi-Fi 稳定优先",
        hint = "水位下限 9 槽（约 33ms），断流应对上限 12 槽",
        summary = "实测驱动的一档（软路由 / 家用 AP 的 Wi-Fi）：到达空隙中位数 20ms、\n" +
                "99 分位 29ms、偶发 40ms 以上。把水位下限锁在 9 槽（约 33ms）直接覆盖 99 分位空隙，\n" +
                "断流应对上限抬到 12 槽（约 44ms），实测可把欠载压到接近零。\n" +
                "代价是网络好转时水位也不会低于 33ms —— 要更低延迟请退回左侧档位。",
        minTargetSlots = 9,
        stallPeakCapSlots = 12.0,
    ),
    WAN(
        label = "公网",
        hint = "容量 60 槽、下限 12 槽（约 44ms），水位上限 40 槽（约 146ms）",
        summary = "跨地域 / VPN / 4G-5G：容量 60 槽（水位上限 40 槽 ≈ 146ms），" +
                "水位下限 12 槽（约 44ms），断流应对上限放到 16 槽（约 58ms）覆盖常态化长间隙，" +
                "断流记忆衰减放慢到 5ms/s，让稀疏断流之间也记得住上次的教训。",
        jbCapacity = 60,
        minTargetSlots = 12,
        stallPeakCapSlots = 16.0,
        stallPeakDecayMsPerSec = 5.0,
    ),
    MS_50(
        label = "50 ms",
        hint = "水位下限锁在 14 槽（约 51ms），上限 40 槽",
        summary = "给水位划一条 50ms 的下限（14 槽），容量 60 槽（上限 40 槽 ≈ 146ms）。\n" +
                "适用于抖动中等且不希望延迟进一步下探的场景。\n" +
                "设定下限后，即使网络状况良好水位也不会低于该值，以固定延迟换取稳定性。",
        jbCapacity = 60,
        minTargetSlots = 14,
        stallPeakCapSlots = 16.0,
        stallPeakDecayMsPerSec = 5.0,
    ),
    MS_100(
        label = "100 ms",
        hint = "水位下限锁在 27 槽（约 98ms），上限 40 槽",
        summary = "在公网档之上把水位下限抬到 27 槽（约 98ms）：" +
                "相比依赖欠载反馈逐次抬升，直接设定下限可减少恢复过程中的欠载次数。\n" +
                "适用于公网且对端时钟漂移较明显的链路。",
        jbCapacity = 60,
        minTargetSlots = 27,
        stallPeakCapSlots = 16.0,
        stallPeakDecayMsPerSec = 5.0,
    ),
    WEAK(
        label = "弱网",
        hint = "容量 80 槽（上限 53 槽 ≈ 193ms），下限 32 槽（约 117ms）",
        summary = "移动网络边缘 / 拥塞 AP：容量 80 槽（水位上限 53 槽 ≈ 193ms），下限 32 槽（约 117ms），" +
                "并把「卡顿后加固步长」翻倍到 2 槽 —— 散点丢包只把到达间隔拉到约 2 个" +
                "包周期，远低于断流判定门槛，断流应对项基本看不见它们，只能靠这条闭环补偿。",
        jbCapacity = 80,
        minTargetSlots = 32,
        stallPeakCapSlots = 16.0,
        stallPeakDecayMsPerSec = 5.0,
        underrunPenaltySlots = 2.0,
    ),
    MS_200(
        label = "200 ms",
        hint = "水位下限锁在 55 槽（约 200ms），上限 80 槽",
        summary = "容量 120 槽、水位下限 55 槽（约 200ms）：持续劣化的链路用它换" +
                "连续播放，代价为固定延迟显著增加。",
        jbCapacity = 120,
        minTargetSlots = 55,
        stallPeakCapSlots = 20.0,
        stallPeakDecayMsPerSec = 5.0,
    ),
    HOSTILE(
        label = "恶劣",
        hint = "容量 160 槽、下限 80 槽（约 292ms），全档位拉满",
        summary = "最后一档：容量 160 槽（水位上限 106 槽 ≈ 387ms），下限 80 槽，" +
                "断流应对 24 槽、记忆衰减 3ms/s、卡顿加固 3 槽。\n" +
                "适用于链路质量极差、以播放连续性优先于实时性的场景（如弱信号下的单向收听）。" +
                "若该档位仍无法满足，则已超出参数可调节范围。",
        jbCapacity = 160,
        minTargetSlots = 80,
        stallPeakCapSlots = 24.0,
        stallPeakDecayMsPerSec = 3.0,
        underrunPenaltySlots = 3.0,
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
    val jitterGain: Double = 0.0, // k：legacy k×J 路径（默认策略已换尾部分位数，k 只在冷启动回退用）
    val minTargetSlots: Int = 0, // target 硬下限（默认 6 槽）；只能抬高，几何地板无条件托底
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
