package com.aquawius.aqua

import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateListOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue

/**
 * 应用级状态持有者：包住 AquaClient，暴露 Compose 可观察的配置 / 运行状态 / 简要日志。
 *
 * - 轮询模型：MainActivity 的 LaunchedEffect 周期调用 [poll] 拉取 state/diagnostics。
 * - 生命周期：connect() 每次都释放旧句柄再新建（C API 一个句柄只 start 一次）。
 * - 连接在后台线程执行（native start 阻塞至 gRPC 完成，最长约 3s），
 *   点击后立即置 connecting=true + STARTING，UI 不冻结、即时反馈"连接中"。
 * - 重连在 UI/Controller 层实现（core 契约为"终态即停"）：poll() 观察到
 *   STOPPED 且非用户主动断开且 autoReconnect 开启时在后台自动重建会话。
 * - 持久化：成功进入播放态后经 [onConnected] 回调保存服务器地址。
 */
class AquaController(
    initialServerIp: String = "192.168.1.100",
    initialJitterBufferSlots: Int = 0,  // 0 = core 默认 30
    initialHelloIntervalMs: Int = 0,    // 0 = core 默认 1000
    initialClientName: String = "aqua_android",
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

    // ---- 设置（MainActivity 在 onStop 持久化）----
    var autoReconnect by mutableStateOf(initialAutoReconnect)
    var keepScreenOn by mutableStateOf(initialKeepScreenOn)
    var allowSimultaneousPlayback by mutableStateOf(initialAllowSimultaneousPlayback)

    // ---- 运行时状态（由 poll() 刷新；connecting 由连接线程维护）----
    var state by mutableStateOf(AquaRuntimeState.CREATED)
        private set
    var isRunning by mutableStateOf(false)
        private set

    /** 后台连接进行中（用户点击或自动重连）。 */
    var connecting by mutableStateOf(false)
        private set
    var lastError by mutableStateOf("")
        private set
    var diagnostics by mutableStateOf<AquaDiagnostics?>(null)
        private set
    var connectResult by mutableStateOf<AquaConnectResult?>(null)
        private set

    /** 本次连接是否曾进入播放态（connect() 时重置）。 */
    private var hasEverPlayed = false

    /** 用户主动断开标志：poll() 的自动重连据此跳过。 */
    private var userDisconnected = false

    /** 上次自动重连尝试时刻（elapsedRealtime；0 = 无）：失败快速返回时按
     *  RECONNECT_MIN_INTERVAL_MS 退避，避免每个 poll tick 都重试轰炸网络。 */
    private var lastReconnectAtMs = 0L

    /** 自动重连处于激活状态（开关开 + 会话非用户主动断开）。 */
    val autoReconnectActive: Boolean
        get() = autoReconnect && !userDisconnected

    /** 首次连接未成功即停止：视为连接失败而非"已停止"（用于状态横幅）。 */
    val connectionFailed: Boolean
        get() = state == AquaRuntimeState.STOPPED && !hasEverPlayed
            && !autoReconnectActive && !connecting

    // ---- 简要日志（App 事件）----
    val log = mutableStateListOf<String>()

    /** 连接前置动作（MainActivity 注入）：请求通知授权、启动前台服务。 */
    var onConnectRequested: (() -> Unit)? = null

    /**
     * 连接（后台线程）：释放旧句柄 → 新建（配置快照）→ start()。
     * 立即返回：connecting=true + STARTING 由调用方即时看到，native 阻塞
     * 不再冻结 UI。重入保护：运行中或连接中忽略。
     *
     * @param userInitiated 用户点击（true）重置退避基准；自动重连传 false。
     */
    fun connect(userInitiated: Boolean = true) {
        if (isRunning || connecting) return
        onConnectRequested?.invoke()

        connecting = true
        if (userInitiated) {
            lastReconnectAtMs = 0L // 手动连接重置退避基准，失败后自动重连可立即介入
        }
        state = AquaRuntimeState.STARTING // 即时反馈：状态横幅显示"连接中"
        appendLog("连接 ${serverIp.trim()}:${rpcPort.toIntOrNull() ?: 50051}")

        // 连接参数快照在主线程捕获（Compose 状态不跨线程读取）。
        val newClient = AquaClient(
            serverIp = serverIp.trim().ifBlank { "127.0.0.1" },
            rpcPort = rpcPort.toIntOrNull()?.takeIf { it in 1..65535 } ?: 50051,
            clientName = clientName.trim().ifBlank { "aqua_android" },
            jitterBufferSlots = jitterBufferSlots,
            helloIntervalMs = helloIntervalMs,
            playbackFramesPerBuffer = 0, // backend 自适应（设计决议）
            forceUdpPort = 0,            // 采用 server 通告
            logLevel = -1,               // 保持进程当前级别（默认 Info）
        )

        Thread {
            try {
                releaseClient() // 释放旧句柄（destroy 含 stop+join，一并后台化）
                hasEverPlayed = false
                userDisconnected = false
                lastError = "" // 新会话开始：清掉上一次连接失败留下的错误信息
                client = newClient

                val rc = newClient.connect()
                appendLog("start → rc=$rc")
                if (rc != AquaClient.STATUS_OK) {
                    appendLog("连接失败: ${newClient.lastAudioErrorName()}")
                }
            } finally {
                connecting = false
            }
        }.start()
    }

    /** 断开（后台线程执行 native stop，非阻塞返回）。
     *  立即置 STOPPED + isRunning=false：poll() 因 client==null 不再刷新，
     *  状态需在此同步落地，否则 UI 卡在"播放中"且 connect() 被守卫拦截。 */
    fun disconnect() {
        if (!isRunning && !connecting) return
        userDisconnected = true
        appendLog("断开连接")
        val c = client
        client = null
        diagnostics = null
        connectResult = null
        isRunning = false
        state = AquaRuntimeState.STOPPED
        Thread {
            c?.stop() // stop 含 join 内部 IO 线程 + gRPC disconnect，一并后台化
        }.start()
    }

    /** 轮询：拉取状态 / 错误 / 诊断；状态迁移写入日志；终态时按设置自动重连。 */
    fun poll() {
        val c = client ?: return

        val s = c.state()
        if (s != state) {
            state = s
            appendLog("状态: ${s.label}")
            if (s == AquaRuntimeState.RUNNING) {
                hasEverPlayed = true
                onConnected(this)
            }
        }

        val running = s == AquaRuntimeState.RUNNING || s == AquaRuntimeState.DEGRADED
        if (running != isRunning) {
            isRunning = running
        }

        val err = c.lastAudioErrorName()
        if (err.isNotEmpty() && err != "none" && err != lastError) {
            lastError = err
            appendLog("错误: $err")
        }

        // 诊断/连接结果每 POLL_TICKS_PER_DIAG（500ms×2=1s）刷新一次：
        // 计数器类指标跳变过快，无观察价值。
        if (++pollTickCount >= POLL_TICKS_PER_DIAG) {
            pollTickCount = 0
            diagnostics = c.diagnostics()
            connectResult = c.connectResult()
        }

        // 自动重连（UI 层实现，core 契约"终态即停"）：会话停止且非用户主动断开。
        // 退避：距上次尝试不足 RECONNECT_MIN_INTERVAL_MS 则本轮跳过（快速失败
        // 场景下避免每个 poll tick 都重试；慢失败场景 gRPC 超时本身已拉开间隔）。
        if (s == AquaRuntimeState.STOPPED && autoReconnectActive && !connecting) {
            val now = android.os.SystemClock.elapsedRealtime()
            if (lastReconnectAtMs == 0L
                || now - lastReconnectAtMs >= RECONNECT_MIN_INTERVAL_MS
            ) {
                lastReconnectAtMs = now
                appendLog("自动重连…")
                connect(userInitiated = false) // 后台线程执行，poll 不阻塞
            }
        }
    }

    companion object {
        /** 自动重连最小间隔：失败快速返回时的重试退避。 */
        private const val RECONNECT_MIN_INTERVAL_MS = 3000L

        /** 诊断刷新节流：每 N 个 poll tick（500ms）刷新一次诊断/连接结果（1s）。 */
        private const val POLL_TICKS_PER_DIAG = 2
    }

    /** poll tick 计数（诊断节流用）。 */
    private var pollTickCount = 0

    /** 恢复高级参数默认值。 */
    fun restoreDefaults() {
        jitterBufferSlots = 0
        helloIntervalMs = 0
        clientName = "aqua_android"
        appendLog("已恢复高级参数默认值")
    }

    /** 释放 native 句柄（后台线程，含隐式 stop）。 */
    fun destroy() {
        val c = client
        client = null
        Thread { c?.destroy() }.start()
    }

    private fun releaseClient() {
        client?.destroy()
        client = null
    }

    private fun appendLog(line: String) {
        log.add(line)
        while (log.size > 500) log.removeAt(0)
    }
}
