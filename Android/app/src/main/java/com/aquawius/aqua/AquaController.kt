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
 * - 持久化：成功进入播放态后经 [onConnected] 回调保存服务器地址。
 */
class AquaController(
    initialServerIp: String = "192.168.1.100",
    initialJitterBufferMs: Int = 0,     // 0 = 默认 30ms；滑块 0..300
    initialJitterDetectWindowPackets: Int = 0, // 0 = 默认 500 包；滑块 0..2000
    initialPlaybackBufferKb: Int = 0,   // 0 = 默认 16KB；滑块 0..400
    initialClientName: String = "aqua_android",
    private val onConnected: (AquaController) -> Unit = {},
) {
    private val client = AquaClient()

    // ---- 可编辑配置（首页 + 高级）----
    var serverIp by mutableStateOf(initialServerIp)
    var rpcPort by mutableStateOf("50051")
    var jitterBufferMs by mutableStateOf(initialJitterBufferMs)
    var jitterDetectWindowPackets by mutableStateOf(initialJitterDetectWindowPackets)
    var playbackBufferKb by mutableStateOf(initialPlaybackBufferKb)
    var clientName by mutableStateOf(initialClientName)

    // ---- 设置 ----
    var autoReconnect by mutableStateOf(false)
    var keepScreenOn by mutableStateOf(false)
    var allowSimultaneousPlayback by mutableStateOf(false)

    // ---- 运行时状态（由 poll() 刷新）----
    var state by mutableStateOf(AquaClientState.IDLE)
        private set
    var isRunning by mutableStateOf(false)
        private set
    var lastError by mutableStateOf("")
        private set
    var diagnostics by mutableStateOf<AquaDiagnostics?>(null)
        private set
    var audioFormat by mutableStateOf<AquaAudioFormat?>(null)
        private set

    /** 本次连接是否曾进入播放态（connect() 时重置）。 */
    private var hasEverPlayed = false

    /** 首次连接未成功即停止：视为连接失败而非"已停止"（用于状态横幅）。 */
    val connectionFailed: Boolean
        get() = state == AquaClientState.STOPPED && !hasEverPlayed

    // ---- 简要日志（App 事件）----
    val log = mutableStateListOf<String>()

    /** 连接前置动作（MainActivity 注入）：请求通知授权、启动前台服务。 */
    var onConnectRequested: (() -> Unit)? = null

    /** 连接：释放旧句柄 → 应用配置 → 新建 → start()。
     *  防重入用 native 真值（isRunning 快照有 250ms 轮询延迟，双击会误重启会话）。 */
    fun connect() {
        if (client.isRunning()) return
        onConnectRequested?.invoke()

        client.destroy() // 幂等：释放上一个（已停止/空闲）句柄
        hasEverPlayed = false
        lastError = "" // 新会话开始：清掉上一次连接失败留下的错误信息
        client.serverIp = serverIp.trim().ifBlank { "127.0.0.1" }
        client.rpcPort = rpcPort.toIntOrNull()?.takeIf { it in 1..65535 } ?: 50051
        // JB 单参数：总量预算，floor/ceiling 由 core 内部推导。
        client.jitterBufferMs = jitterBufferMs
        client.jitterDetectWindowPackets = jitterDetectWindowPackets
        client.playbackBufferSize = playbackBufferKb * 1024L
        client.autoReconnect = autoReconnect
        client.clientName = clientName.trim().ifBlank { "aqua_android" }

        client.create()
        val rc = client.start()
        appendLog("连接 ${client.serverIp}:${client.rpcPort} → rc=$rc")
        if (rc != AquaClient.STATUS_OK) {
            appendLog("start 失败: ${client.lastError()}")
        }
    }

    /** 断开（优雅关闭，非阻塞）。未在运行时忽略，避免误导日志。 */
    fun disconnect() {
        if (!client.isRunning()) return
        client.shutdown()
        appendLog("断开连接")
    }

    /** 轮询：拉取状态 / 错误 / 诊断，状态迁移写入日志，成功播放时回调持久化。 */
    fun poll() {
        val s = client.state()
        if (s != state) {
            val prev = state
            state = s
            appendLog("状态: $s")
            if (s == AquaClientState.PLAYING) {
                hasEverPlayed = true
                if (prev != AquaClientState.PLAYING) {
                    onConnected(this)
                }
            }
        }
        val running = client.isRunning()
        if (running != isRunning) {
            isRunning = running
        }
        val err = client.lastError()
        if (err.isNotEmpty() && err != lastError) {
            lastError = err
            appendLog("错误: $err")
        }
        diagnostics = client.diagnostics()
        audioFormat = client.audioFormat()
    }

    /** 恢复高级参数默认值。 */
    fun restoreDefaults() {
        jitterBufferMs = 0
        jitterDetectWindowPackets = 0
        playbackBufferKb = 0
        clientName = "aqua_android"
        appendLog("已恢复高级参数默认值")
    }

    /** 释放 native 句柄。 */
    fun destroy() {
        client.destroy()
    }

    private fun appendLog(line: String) {
        log.add(line)
        while (log.size > 500) log.removeAt(0)
    }
}
