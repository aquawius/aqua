package com.aquawius.aqua

import android.os.Handler
import android.os.Looper
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateListOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import java.util.concurrent.ExecutorService
import java.util.concurrent.Executors
import java.util.concurrent.atomic.AtomicBoolean

/**
 * 应用级状态持有者：包住 AquaClient，暴露 Compose 可观察的配置 / 运行状态 / 简要日志。
 *
 * - 轮询模型：MainActivity 的 LaunchedEffect 周期调用 [poll] 拉取 state/diagnostics。
 * - 生命周期：connect() 每次都释放旧句柄再新建（C API 一个句柄只 start 一次）。
 * - native 串行化：create/start/stop/destroy 与 poll 的 native 读全部经
 *   [lifecycleExecutor]（单线程）排队执行，满足 C API"生命周期需串行调用"契约
 *   （老项目靠主线程同步调用躲开了此坑——代价是 UI 冻结；这里两全）。
 * - 连接在后台线程执行（native start 阻塞至 gRPC 完成，最长约 3s），
 *   点击后立即置 connecting=true + STARTING，UI 不冻结、即时反馈"连接中"。
 * - 重连在 UI/Controller 层实现（core 契约为"终态即停"）：
 *   播放异常停止（STOPPED 且非用户主动断开）→ 横幅显示"已停止" →
 *   [RECONNECT_DELAY_MS] 后（开关开启时）后台重建会话。停止期可见，
 *   快速失败与慢失败统一退避。
 * - 持久化：成功进入播放态后经 [onConnected] 回调保存服务器地址。
 */
class AquaController(
    initialServerIp: String = "192.168.1.100",
    initialJitterBufferSlots: Int = 0,  // 0 = core 默认 30
    initialHelloIntervalMs: Int = 0,    // 0 = core 默认 1000
    initialClientName: String = "aqua_android",
    initialForceUdpPort: String = "",   // 空 = 0 = server 通告值
    initialLogLevel: Int = -1,          // -1 = 默认（Info）
    initialAutoReconnect: Boolean = false,
    initialKeepScreenOn: Boolean = false,
    initialAllowSimultaneousPlayback: Boolean = false,
    private val onConnected: (AquaController) -> Unit = {},
) {
    private var client: AquaClient? = null

    // ---- 可编辑配置（首页 + 高级）----
    var serverIp by mutableStateOf(initialServerIp)
    var rpcPort by mutableStateOf("50051")
    var jitterBufferSlots by mutableStateOf(initialJitterBufferSlots)
    var helloIntervalMs by mutableStateOf(initialHelloIntervalMs)
    var clientName by mutableStateOf(initialClientName)

    /** UDP 端口覆盖（CLI --force-udp-port）：空/0 = server 通告值；NAT/端口映射用。 */
    var forceUdpPort by mutableStateOf(initialForceUdpPort)

    /** 日志级别（CLI --log-level）：-1 = 默认（Info）；0..5 = Trace..Fatal。 */
    var logLevel by mutableStateOf(initialLogLevel)

    // ---- 设置（MainActivity 在 onStop 持久化）----
    var autoReconnect by mutableStateOf(initialAutoReconnect)
    var keepScreenOn by mutableStateOf(initialKeepScreenOn)
    var allowSimultaneousPlayback by mutableStateOf(initialAllowSimultaneousPlayback)

    // ---- 运行时状态（由 poll() 刷新；connecting/reconnecting 由连接路径维护）----
    var state by mutableStateOf(AquaRuntimeState.CREATED)
        private set
    var isRunning by mutableStateOf(false)
        private set

    /** 后台连接进行中（用户点击或自动重连）。 */
    var connecting by mutableStateOf(false)
        private set

    /** 后台停止进行中（native stop 含 join IO 线程，点击断开后置位）。 */
    var stopping by mutableStateOf(false)
        private set

    /** 本次连接由自动重连发起（横幅区分"连接中"与"自动重连中"）。 */
    var reconnecting by mutableStateOf(false)
        private set
    var lastError by mutableStateOf("")
        private set
    var diagnostics by mutableStateOf<AquaDiagnostics?>(null)
        private set
    var connectResult by mutableStateOf<AquaConnectResult?>(null)
        private set

    /** 会话时长（ms）：进入播放起累计，停止后冻结；未播放过为 null。 */
    var sessionDurationMs by mutableStateOf<Long?>(null)
        private set

    /** 本次连接是否曾进入播放态（connect() 时重置）。 */
    private var hasEverPlayed = false

    /** 用户主动断开标志：poll() 的自动重连据此跳过；connect 任务据此放弃/中止。 */
    @Volatile
    private var userDisconnected = false

    /** native 生命周期串行执行器：C API 契约要求 create/start/stop/destroy
     *  串行调用；poll 的 native 读也入队，避免与 destroy 并发（UAF）。 */
    private val lifecycleExecutor: ExecutorService =
        Executors.newSingleThreadExecutor { r ->
            Thread(r, "aqua-lifecycle").apply { isDaemon = true }
        }

    /** poll 合并标志：executor 忙（如 3s 阻塞 start）时跳过堆积的 poll 提交。 */
    private val pollPending = AtomicBoolean(false)

    /** 主线程调度：自动重连从 executor 线程发起时，connect() 必须回主线程执行
     *  （onConnectRequested 会触碰 Activity Result API；参数快照读 Compose 状态）。 */
    private val mainHandler = Handler(Looper.getMainLooper())

    /** 异常停止检出时刻（elapsedRealtime；0 = 无）：停止期可见 + 重连退避基准。 */
    private var stopDetectedAtMs = 0L

    /** 本次会话进入播放态的时刻（elapsedRealtime；0 = 未播放过）。 */
    private var sessionStartMs = 0L

    /** 自动重连处于激活状态（开关开 + 会话非用户主动断开）。 */
    val autoReconnectActive: Boolean
        get() = autoReconnect && !userDisconnected

    /** 首次连接未成功即停止：视为连接失败而非"已停止"（用于状态横幅）。
     *  create 失败时 handle=0、state 停在 CREATED，也计入（否则横幅误显"未连接"）。 */
    val connectionFailed: Boolean
        get() = connectAttempted && !hasEverPlayed && !connecting && !stopping &&
            (state == AquaRuntimeState.STOPPED || state == AquaRuntimeState.CREATED)

    /** 是否发起过连接（连接失败判定用；横幅"连接失败"需至少尝试过一次）。 */
    private var connectAttempted = false

    // ---- 简要日志（App 事件）----
    val log = mutableStateListOf<String>()

    /** 连接前置动作（MainActivity 注入）：请求通知授权、启动前台服务。 */
    var onConnectRequested: (() -> Unit)? = null

    /**
     * 连接（后台线程）：释放旧句柄 → 新建（配置快照）→ start()。
     * 立即返回：connecting=true + STARTING 由调用方即时看到，native 阻塞
     * 不再冻结 UI。重入保护：运行中或连接中忽略。
     *
     * @param reconnect 自动重连发起（true）：横幅显示"自动重连中"；
     *                  用户点击传 false，横幅显示"连接中"。
     */
    fun connect(reconnect: Boolean = false) {
        if (isRunning || connecting || stopping) return

        // ---- 参数前置校验（core 对非法配置只写日志、不设 AudioError，
        // ---- 若不提前拒绝，App 只能兜底显示"无法连接服务器"，无法反馈真实原因）。
        if (jitterBufferSlots in 1 until CORE_MIN_JITTER_BUFFER_SLOTS) {
            val msg = "参数无效：抖动缓冲槽数 $jitterBufferSlots 低于最小值 $CORE_MIN_JITTER_BUFFER_SLOTS（0 = 默认 30）"
            lastError = msg
            connectAttempted = true // 横幅显示"连接失败" + 具体原因
            appendLog(msg)
            return
        }
        onConnectRequested?.invoke()

        connecting = true
        reconnecting = reconnect
        connectAttempted = true
        // 主线程同步清除断开标志：此后若置位必是"本次连接期间用户点了断开"，
        // connect 任务据此放弃（排队期）或启动后立即停止（start 完成后）。
        userDisconnected = false
        stopDetectedAtMs = 0L // 进入新会话，清除停止检出
        sessionStartMs = 0L   // 会话时长重新起算
        sessionDurationMs = null
        state = AquaRuntimeState.STARTING // 即时反馈：状态横幅显示"连接中"
        appendLog(
            if (reconnect) "自动重连 ${serverIp.trim()}" else "连接 ${serverIp.trim()}:${rpcPort.toIntOrNull() ?: 50051}",
        )

        // 连接参数快照在主线程捕获（Compose 状态不跨线程读取）。
        val newClient = AquaClient(
            serverIp = serverIp.trim().ifBlank { "127.0.0.1" },
            rpcPort = rpcPort.toIntOrNull()?.takeIf { it in 1..65535 } ?: 50051,
            clientName = clientName.trim().ifBlank { "aqua_android" },
            jitterBufferSlots = jitterBufferSlots,
            helloIntervalMs = helloIntervalMs,
            playbackFramesPerBuffer = 0, // backend 自适应（设计决议）
            forceUdpPort = forceUdpPort.trim().toIntOrNull()
                ?.takeIf { it in 1..65535 } ?: 0, // 0 = server 通告；非法输入同 0
            logLevel = logLevel,         // -1 = 保持进程当前级别（默认 Info）
        )

        lifecycleExecutor.execute {
            try {
                // 排队期间用户已点断开：放弃本次连接（否则会话凭空播放）。
                if (userDisconnected) {
                    appendLog("连接已取消")
                    return@execute
                }
                releaseClient() // 释放旧句柄（destroy 含 stop+join，一并串行化）
                hasEverPlayed = false
                lastError = "" // 新会话开始：清掉上一次连接失败留下的错误信息
                client = newClient

                val rc = newClient.connect()
                appendLog("start → rc=$rc")
                if (rc != AquaClient.STATUS_OK) {
                    // 失败原因（老版行为：让用户看到具体错误）：
                    // C API 错误名只覆盖 AudioError，gRPC 层失败兜底为网络文案。
                    val err = newClient.lastAudioError()
                    lastError = if (err != AquaAudioError.NONE) {
                        err.label
                    } else {
                        "无法连接服务器 ${newClient.serverIp}:${newClient.rpcPort}"
                    }
                    appendLog("连接失败: $lastError")
                } else if (userDisconnected) {
                    // start 阻塞期间用户点了断开：立即停掉刚建立的会话。
                    newClient.stop()
                    appendLog("连接已被取消")
                }
            } finally {
                connecting = false
                reconnecting = false
            }
        }
    }

    /** 断开（后台线程执行 native stop，非阻塞返回）。
     *  沿用老版句柄模型：stop 后句柄保留（state()→STOPPED，audioFormat 等
     *  查询仍可用，数据不残留不冻结），下次 connect() 时释放重建。
     *  立即置 isRunning=false：poll() 继续从句柄读取 STOPPED 状态落地。 */
    fun disconnect() {
        if (!isRunning && !connecting && !stopping) return
        userDisconnected = true
        stopping = true
        appendLog("断开连接")
        isRunning = false
        lifecycleExecutor.execute {
            try {
                // 执行时读当前句柄（排队期间 connect 任务可能已换新句柄；
                // stop 幂等，与 connect 任务内的取消停止重叠无害）。
                client?.stop() // stop 含 join 内部 IO 线程 + gRPC disconnect，一并串行化
            } finally {
                stopping = false
            }
        }
    }

    /** 轮询：拉取状态 / 错误 / 诊断；状态迁移写入日志；终态时按设置自动重连。
     *  native 读入队（与生命周期串行，避免查询已销毁句柄）；executor 忙时
     *  合并提交，不堆积。 */
    fun poll() {
        if (!pollPending.compareAndSet(false, true)) return
        lifecycleExecutor.execute {
            try {
                pollLocked()
            } finally {
                pollPending.set(false)
            }
        }
    }

    private fun pollLocked() {
        val c = client ?: return

        val s = c.state()
        if (s != state) {
            state = s
            appendLog("状态: ${s.label}")
            if (s == AquaRuntimeState.RUNNING) {
                hasEverPlayed = true
                if (sessionStartMs == 0L) {
                    sessionStartMs = android.os.SystemClock.elapsedRealtime()
                }
                onConnected(this)
            }
            if (s == AquaRuntimeState.STOPPED && !userDisconnected) {
                // 异常停止（用户没点断开）：记录检出时刻，进入可见的停止期。
                stopDetectedAtMs = android.os.SystemClock.elapsedRealtime()
                // 停止原因（老版行为：出错要让用户看到是什么错）。
                // 时机关键：必须在句柄被下一次 connect() 释放前读到。
                val reason = stopReasonOf(c)
                if (reason.isNotEmpty() && hasEverPlayed) {
                    lastError = reason
                    appendLog("错误: $reason")
                }
            }
        }

        val running = s == AquaRuntimeState.RUNNING || s == AquaRuntimeState.DEGRADED
        // stopping 期间不回翻 isRunning：poll 状态快照滞后于后台 stop，避免按钮闪回。
        if (running != isRunning && !stopping) {
            isRunning = running
        }
        if (running && sessionStartMs != 0L) {
            sessionDurationMs = android.os.SystemClock.elapsedRealtime() - sessionStartMs
        }

        // 自动重连（UI 层实现，core 契约"终态即停"）：
        // 播放异常停止 → 停止期（横幅"已停止"）→ 开关开启 → 到期后台重连。
        // RECONNECT_DELAY_MS 统一快/慢失败的退避节奏，且保证"已停止"可见。
        if (s == AquaRuntimeState.STOPPED && autoReconnectActive && !connecting
            && stopDetectedAtMs != 0L
        ) {
            val now = android.os.SystemClock.elapsedRealtime()
            if (now - stopDetectedAtMs >= RECONNECT_DELAY_MS) {
                stopDetectedAtMs = 0L
                // 回主线程执行 connect（Activity Result API + Compose 快照均要求）；
                // 入队不阻塞 poll 任务，主线程的 connect 再把 native 活派回 executor。
                mainHandler.post { connect(reconnect = true) }
            }
        }

        // 已停止：连接结果失效（音频卡回落 "—"，同老版 handle==0 语义）。
        // 诊断保留旧值不再刷新——重连前留最后现场供查看。
        if (s == AquaRuntimeState.STOPPED) {
            connectResult = null
            return
        }

        // 播放中的设备类错误（枚举中文标签，同老版"遇到错误显示是什么错"）。
        val err = c.lastAudioError()
        if (err != AquaAudioError.NONE && err.label != lastError) {
            lastError = err.label
            appendLog("错误: ${err.label}")
        }

        // 诊断/连接结果每 POLL_TICKS_PER_DIAG（500ms×2=1s）刷新一次：
        // 计数器类指标跳变过快，无观察价值。
        if (++pollTickCount >= POLL_TICKS_PER_DIAG) {
            pollTickCount = 0
            diagnostics = c.diagnostics()
            connectResult = c.connectResult()
        }
    }

    companion object {
        /** 自动重连延迟（停止检出起算）：停止期可见 + 失败重试退避。 */
        private const val RECONNECT_DELAY_MS = 3000L

        /** 诊断刷新节流：每 N 个 poll tick（500ms）刷新一次诊断/连接结果（1s）。 */
        private const val POLL_TICKS_PER_DIAG = 2

        /** core JITTER_BUFFER_MIN_CAPACITY_SLOTS：显式槽数的合法下界（0 = 默认 30）。 */
        private const val CORE_MIN_JITTER_BUFFER_SLOTS = 4
    }

    /** poll tick 计数（诊断节流用）。 */
    private var pollTickCount = 0

    /** 恢复高级参数默认值。 */
    fun restoreDefaults() {
        jitterBufferSlots = 0
        helloIntervalMs = 0
        clientName = "aqua_android"
        forceUdpPort = ""
        logLevel = -1
        appendLog("已恢复高级参数默认值")
    }

    /** 释放 native 句柄（串行队列，含隐式 stop；排在在途 connect 任务之后）。 */
    fun destroy() {
        lifecycleExecutor.execute {
            releaseClient()
        }
    }

    private fun releaseClient() {
        client?.destroy()
        client = null
    }

    /** 停止原因：音频错误优先，其次 HELLO 失败（读一次终态诊断），最后泛化描述。 */
    private fun stopReasonOf(c: AquaClient): String {
        val err = c.lastAudioError()
        if (err != AquaAudioError.NONE) return err.label
        if (c.diagnostics()?.helloFailed == true) return "服务器无响应（HELLO 失败）"
        return "连接已中断"
    }

    private fun appendLog(line: String) {
        log.add(line)
        while (log.size > 500) log.removeAt(0)
    }
}
