package com.aquawius.aqua

import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateListOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue

/**
 * 应用级状态持有者：包住 AquaClient，暴露 Compose 可观察的配置 / 运行状态 / 事件日志。
 *
 * M1 为轮询模型：MainActivity 的 LaunchedEffect 周期调用 [poll] 拉取 state/diagnostics。
 * 生命周期：connect() 每次都释放旧句柄再新建（C API 一个句柄只 start 一次）。
 */
class AquaController {
    private val client = AquaClient()

    // ---- 可编辑配置（首页 + 高级）----
    var serverIp by mutableStateOf("192.168.1.100")
    var rpcPort by mutableStateOf("50051")
    var jitterLatencyMs by mutableStateOf("0")   // 0 = 默认 30ms
    var driftThreshold by mutableStateOf("0")    // 0 = 默认 15
    var playbackBufferBytes by mutableStateOf("0") // 0 = 默认 16KB

    // ---- 设置 ----
    var autoReconnect by mutableStateOf(false)
    var keepScreenOn by mutableStateOf(false)
    var ignoreBatteryOptimization by mutableStateOf(false)

    // ---- 运行时状态（由 poll() 刷新）----
    var state by mutableStateOf(AquaClientState.IDLE)
        private set
    var isRunning by mutableStateOf(false)
        private set
    var lastError by mutableStateOf("")
        private set
    var diagnostics by mutableStateOf<AquaDiagnostics?>(null)
        private set

    // ---- 事件日志（高级页底部展示）----
    val log = mutableStateListOf<String>()

    /** 连接：释放旧句柄 → 应用配置 → 新建 → start()。 */
    fun connect() {
        if (isRunning) return

        client.destroy() // 幂等：释放上一个（已停止/空闲）句柄
        client.serverIp = serverIp.trim().ifBlank { "127.0.0.1" }
        client.rpcPort = rpcPort.toIntOrNull() ?: 50051
        client.jitterLatencyMs = jitterLatencyMs.toIntOrNull() ?: 0
        client.driftThreshold = driftThreshold.toIntOrNull() ?: 0
        client.playbackBufferSize = playbackBufferBytes.toLongOrNull() ?: 0
        client.autoReconnect = autoReconnect

        client.create()
        val rc = client.start()
        appendLog("连接 ${client.serverIp}:${client.rpcPort} → rc=$rc")
        if (rc != AquaClient.STATUS_OK) {
            appendLog("start 失败: ${client.lastError()}")
        }
    }

    /** 断开（优雅关闭，非阻塞）。 */
    fun disconnect() {
        client.shutdown()
        appendLog("断开连接")
    }

    /** 轮询：拉取状态 / 错误 / 诊断，更新可观察状态，状态迁移写入日志。 */
    fun poll() {
        val s = client.state()
        if (s != state) {
            state = s
            appendLog("状态: $s")
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
    }

    /** 释放 native 句柄。 */
    fun destroy() {
        client.destroy()
    }

    private fun appendLog(line: String) {
        log.add(line)
        while (log.size > 500) log.removeAt(0) // 限制条数，避免无限增长
    }
}
