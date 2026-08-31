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
 * - 重连在 UI/Controller 层实现（core 契约为"终态即停"）：poll() 观察到
 *   STOPPED 且非用户主动断开且 autoReconnect 开启时自动重建会话。
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

    // ---- 运行时状态（由 poll() 刷新）----
    var state by mutableStateOf(AquaRuntimeState.CREATED)
        private set
    var isRunning by mutableStateOf(false)
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

    /** 自动重连进行中（用于状态横幅显示，避免误报"连接失败"）。 */
    var autoReconnecting by mutableStateOf(false)
        private set

    /** 上次自动重连尝试时刻（SystemClock；0 = 无）：失败快速返回时按
     *  RECONNECT_MIN_INTERVAL_MS 退避，避免每个 poll tick 都重试轰炸网络。 */
    private var lastReconnectAtMs = 0L

    /** 首次连接未成功即停止：视为连接失败而非"已停止"（用于状态横幅）。 */
    val connectionFailed: Boolean
        get() = state == AquaRuntimeState.STOPPED && !hasEverPlayed && !autoReconnecting

    // ---- 简要日志（App 事件）----
    val log = mutableStateListOf<String>()

    /** 连接前置动作（MainActivity 注入）：请求通知授权、启动前台服务。 */
    var onConnectRequested: (() -> Unit)? = null

    /** 连接：释放旧句柄 → 新建（配置快照）→ start()（阻塞至 gRPC 完成）。
     *  注意：不重置 autoReconnecting —— 该标志由发起方（用户或 poll 的重连）管理，
     *  重连期间保持 true 以显示横幅。 */
    fun connect() {
        if (isRunning) return
        onConnectRequested?.invoke()

        releaseClient()
        hasEverPlayed = false
        userDisconnected = false
        lastReconnectAtMs = 0L // 手动连接重置退避基准，失败后自动重连可立即介入
        lastError = "" // 新会话开始：清掉上一次连接失败留下的错误信息

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
        client = newClient

        val rc = newClient.connect()
        appendLog("连接 ${newClient.serverIp}:${newClient.rpcPort} → rc=$rc")
        if (rc != AquaClient.STATUS_OK) {
            appendLog("start 失败: ${newClient.lastAudioErrorName()}")
        }
    }

    /** 断开（优雅关闭，非阻塞）。未在运行时忽略，避免误导日志。 */
    fun disconnect() {
        if (!isRunning) return
        userDisconnected = true
        client?.stop()
        appendLog("断开连接")
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

        diagnostics = c.diagnostics()
        connectResult = c.connectResult()

        // 自动重连（UI 层实现，core 契约"终态即停"）：会话停止且非用户主动断开。
        // 退避：距上次尝试不足 RECONNECT_MIN_INTERVAL_MS 则本轮跳过（快速失败
        // 场景下避免每个 poll tick 都重试；慢失败场景 gRPC 超时本身已拉开间隔）。
        if (s == AquaRuntimeState.STOPPED && !userDisconnected && autoReconnect
            && !autoReconnecting
        ) {
            val now = android.os.SystemClock.elapsedRealtime()
            if (lastReconnectAtMs == 0L || now - lastReconnectAtMs >= RECONNECT_MIN_INTERVAL_MS) {
                lastReconnectAtMs = now
                autoReconnecting = true
                appendLog("自动重连…")
                try {
                    connect()
                } finally {
                    autoReconnecting = false
                }
            }
        }
    }

    companion object {
        /** 自动重连最小间隔：失败快速返回时的重试退避。 */
        private const val RECONNECT_MIN_INTERVAL_MS = 3000L
    }

    /** 恢复高级参数默认值。 */
    fun restoreDefaults() {
        jitterBufferSlots = 0
        helloIntervalMs = 0
        clientName = "aqua_android"
        appendLog("已恢复高级参数默认值")
    }

    /** 释放 native 句柄。 */
    fun destroy() {
        releaseClient()
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
