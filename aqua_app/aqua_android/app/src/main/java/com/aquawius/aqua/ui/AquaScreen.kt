package com.aquawius.aqua.ui

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Autorenew
import androidx.compose.material.icons.filled.CheckCircle
import androidx.compose.material.icons.filled.Dns
import androidx.compose.material.icons.filled.Error
import androidx.compose.material.icons.filled.GraphicEq
import androidx.compose.material.icons.filled.Info
import androidx.compose.material.icons.filled.Insights
import androidx.compose.material.icons.filled.Memory
import androidx.compose.material.icons.filled.NetworkCheck
import androidx.compose.material.icons.filled.PlayArrow
import androidx.compose.material.icons.filled.Stop
import androidx.compose.material.icons.filled.Storage
import androidx.compose.material.icons.filled.Tag
import androidx.compose.material3.Button
import androidx.compose.material3.FilledTonalButton
import androidx.compose.material3.Icon
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedCard
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.tooling.preview.Preview
import androidx.compose.ui.unit.dp
import com.aquawius.aqua.AquaConnectResult
import com.aquawius.aqua.AquaController
import com.aquawius.aqua.AquaDiagnostics
import com.aquawius.aqua.AquaRuntimeState
import com.aquawius.aqua.formatHostPort
import com.aquawius.aqua.ui.theme.AquaTheme
import java.util.Locale
import kotlin.math.roundToInt

/** 首页：地址 + gRPC 端口 + 面向用户的核心指标（音频契约 / 连接质量 / 缓冲水位）。
 *  完整开发诊断在"高级"页；此处只保留用户关心的少数指标。 */
@Composable
fun AquaScreen(controller: AquaController, modifier: Modifier = Modifier) {
    Column(
        modifier = modifier
            .fillMaxSize()
            .padding(16.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        Column(
            modifier = Modifier
                .weight(1f)
                .verticalScroll(rememberScrollState()),
            verticalArrangement = Arrangement.spacedBy(12.dp),
        ) {
            // 连接中/停止中也锁定：连接参数快照在点击瞬间已捕获，此时编辑不生效。
            val inputsLocked = controller.isRunning || controller.connecting || controller.stopping
            OutlinedTextField(
                value = controller.serverIp,
                onValueChange = { controller.serverIp = it },
                label = { Text("服务器地址") },
                singleLine = true,
                enabled = !inputsLocked,
                leadingIcon = { Icon(Icons.Filled.Dns, contentDescription = null) },
                modifier = Modifier.fillMaxWidth(),
            )
            OutlinedTextField(
                value = controller.rpcPort,
                onValueChange = { controller.rpcPort = it.filter { c -> c.isDigit() } },
                label = { Text("gRPC 端口") },
                singleLine = true,
                enabled = !inputsLocked,
                leadingIcon = { Icon(Icons.Filled.Tag, contentDescription = null) },
                keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number),
                modifier = Modifier.fillMaxWidth(),
            )

            MetricsSection(
                controller.state,
                controller.connecting,
                controller.diagnostics,
                controller.connectResult,
                controller.sessionDurationMs,
            )
        }

        // 底部固定区：状态横幅紧贴连接按钮上方。
        StatusBanner(controller)
        ConnectButton(controller)
    }
}

private data class StatusStyle(
    val container: Color,
    val onContainer: Color,
    val icon: ImageVector,
)

/** 状态横幅语义链（优先级从高到低）：
 *  停止中(stopping：后台 stop 进行中) → 播放中(RUNNING/DEGRADED) →
 *  连接中(connecting：手动点击 or 自动重连) →
 *  已停止·将重连(异常停止 + 开关开，3s 停止期) → 连接失败(首次未成功) →
 *  已停止(手动断开) / 未连接。 */
@Composable
private fun StatusBanner(controller: AquaController) {
    val scheme = MaterialTheme.colorScheme
    val state = controller.state
    val style = when {
        controller.stopping -> StatusStyle(
            scheme.secondaryContainer, scheme.onSecondaryContainer, Icons.Filled.Autorenew,
        )
        state == AquaRuntimeState.RUNNING || state == AquaRuntimeState.DEGRADED -> StatusStyle(
            scheme.primaryContainer, scheme.onPrimaryContainer, Icons.Filled.CheckCircle,
        )
        controller.connecting -> StatusStyle(
            scheme.tertiaryContainer, scheme.onTertiaryContainer, Icons.Filled.Autorenew,
        )
        state == AquaRuntimeState.STOPPED && controller.autoReconnectActive -> StatusStyle(
            scheme.secondaryContainer, scheme.onSecondaryContainer, Icons.Filled.Autorenew,
        )
        controller.connectionFailed -> StatusStyle(
            scheme.errorContainer, scheme.onErrorContainer, Icons.Filled.Error,
        )
        else -> StatusStyle(
            scheme.secondaryContainer, scheme.onSecondaryContainer, Icons.Filled.Info,
        )
    }
    val label = when {
        controller.stopping -> "断开中"
        controller.connecting && controller.reconnecting -> "自动重连中"
        controller.connecting -> "连接中"
        state == AquaRuntimeState.RUNNING || state == AquaRuntimeState.DEGRADED -> state.label
        state == AquaRuntimeState.STOPPED && controller.autoReconnectActive -> "已停止 · 将自动重连"
        controller.connectionFailed -> "连接失败"
        else -> state.label
    }
    Surface(
        color = style.container,
        contentColor = style.onContainer,
        shape = MaterialTheme.shapes.large,
    ) {
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .padding(horizontal = 16.dp, vertical = 12.dp),
            horizontalArrangement = Arrangement.spacedBy(12.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Icon(style.icon, contentDescription = null)
            Column(
                Modifier.weight(1f),
                verticalArrangement = Arrangement.spacedBy(2.dp),
            ) {
                Text(
                    "连接状态：$label",
                    style = MaterialTheme.typography.titleSmall,
                    fontWeight = FontWeight.SemiBold,
                )
                if (controller.lastError.isNotEmpty()) {
                    Text(
                        controller.lastError,
                        style = MaterialTheme.typography.bodySmall,
                        maxLines = 2,
                        overflow = TextOverflow.Ellipsis,
                    )
                }
            }
        }
    }
}

/** 主操作按钮：文案恒定（连接 / 断开连接），进行时仅禁用防重入；
 *  过程状态（连接中/断开中）由上方状态横幅反馈，不占按钮文案。 */
@Composable
private fun ConnectButton(controller: AquaController) {
    if (controller.isRunning) {
        FilledTonalButton(
            onClick = { controller.disconnect() },
            enabled = !controller.stopping,
            modifier = Modifier
                .fillMaxWidth()
                .height(52.dp),
        ) {
            Icon(Icons.Filled.Stop, contentDescription = null)
            Spacer(Modifier.width(8.dp))
            Text("断开连接")
        }
    } else {
        Button(
            onClick = { controller.connect() },
            enabled = !controller.connecting && !controller.stopping,
            modifier = Modifier
                .fillMaxWidth()
                .height(52.dp),
        ) {
            Icon(Icons.Filled.PlayArrow, contentDescription = null)
            Spacer(Modifier.width(8.dp))
            Text("连接")
        }
    }
}

/** 核心指标（面向用户精选，布局同老版）：
 *  音频契约卡恒显（未连接时占位 "—"）；
 *  连接/播放中但诊断未到 → "正在收集数据…"；
 *  已连接 → 连接 + 传输 + 缓冲水位 + 播放消费；空闲 → 引导占位。 */
@Composable
private fun MetricsSection(
    state: AquaRuntimeState,
    connecting: Boolean,
    d: AquaDiagnostics?,
    format: AquaConnectResult?,
    sessionDurationMs: Long?,
) {
    // 音频卡固定占位，未连接时全部显示 "—"（同老版）。
    MetricGroupCard("音频", Icons.Filled.GraphicEq, audioMetrics(format, d))

    if (d != null && format != null) {
        MetricGroupCard(
            "连接",
            Icons.Filled.NetworkCheck,
            qualityMetrics(d, format, sessionDurationMs),
        )
        MetricGroupCard("传输", Icons.Filled.Dns, transportMetrics(d))
        MetricGroupCard(
            title = "缓冲",
            icon = Icons.Filled.Storage,
            metrics = bufferMetrics(d),
            progress = d.jbWaterLevel.toFloat(),
        )
        MetricGroupCard("播放消费", Icons.Filled.Memory, playbackMetrics(d))
        return
    }
    // 连接中/播放中但首个诊断周期未到：视为收集中，避免闪现默认占位（同老版）。
    when {
        connecting -> PlaceholderCard("正在收集数据…")
        state == AquaRuntimeState.STARTING -> PlaceholderCard("正在收集数据…")
        state == AquaRuntimeState.RUNNING || state == AquaRuntimeState.DEGRADED ->
            PlaceholderCard("正在收集数据…")
        else -> PlaceholderCard("连接后此处显示实时指标")
    }
}

/** 占位卡：图标 + 提示文案。 */
@Composable
private fun PlaceholderCard(text: String) {
    OutlinedCard(Modifier.fillMaxWidth()) {
        Column(
            modifier = Modifier
                .fillMaxWidth()
                .padding(vertical = 32.dp),
            horizontalAlignment = Alignment.CenterHorizontally,
            verticalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            Icon(
                Icons.Filled.Insights,
                contentDescription = null,
                tint = MaterialTheme.colorScheme.onSurfaceVariant,
                modifier = Modifier.size(32.dp),
            )
            Text(
                text,
                style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
    }
}

private fun audioMetrics(f: AquaConnectResult?, d: AquaDiagnostics?): List<MetricEntry> {
    if (f == null) {
        return listOf(
            MetricEntry("采样率", "—"),
            MetricEntry("声道", "—"),
            MetricEntry("编码", "—"),
            MetricEntry("位深", "—"),
            MetricEntry("码率", "—"),
            MetricEntry("帧长 F", "—"),
        ) + streamMetrics(d)
    }
    val sampleRateText = if (f.sampleRate % 1000 == 0) {
        "${f.sampleRate / 1000} kHz"
    } else {
        String.format(Locale.US, "%.1f kHz", f.sampleRate / 1000.0)
    }
    val channelsText = when (f.channels) {
        1 -> "单声道"
        2 -> "立体声"
        else -> "${f.channels} 声道"
    }
    return listOf(
        MetricEntry("采样率", sampleRateText),
        MetricEntry("声道", channelsText),
        MetricEntry("编码", f.encoding.label),
        MetricEntry("位深", if (f.encoding.bitsPerSample > 0) "${f.encoding.bitsPerSample} bit" else "—"),
        MetricEntry("码率", if (f.bitRateKbps > 0) "${f.bitRateKbps} kbps" else "—"),
        MetricEntry("帧长 F", if (f.frameCount > 0) {
            String.format(Locale.US, "%.1f ms", f.frameCount * 1000.0 / f.sampleRate)
        } else {
            "—"
        }),
    ) + streamMetrics(d)
}

/** 输出流实际运行参数（后端 open 后回读；未连接/未开始时 "—"）。
 *  性能模式（AAudio 原值）：10=无 11=省电 12=低延迟；WASAPI 12=低延迟(IAudioClient3) 10=标准。
 *  不显示后端类型（仅 AAudio/WASAPI 两个后端，由平台决定）；缓冲只显示容量
 *  （策略 = 永远填满设备缓冲，size 恒等于容量）。 */
private fun streamMetrics(d: AquaDiagnostics?): List<MetricEntry> {
    if (d == null || d.streamBackend == 0) {
        return listOf(
            MetricEntry("性能模式", "—"),
            MetricEntry("Burst", "—"),
            MetricEntry("设备缓冲容量", "—"),
        )
    }
    val performance = when (d.streamPerformanceMode) {
        12 -> "低延迟"
        11 -> "省电"
        10 -> if (d.streamBackend == 2) "标准" else "无"
        else -> "—"
    }
    val burst = if (d.streamFramesPerBurst > 0) "${d.streamFramesPerBurst} 帧" else "—"
    val capacity = if (d.streamBufferCapacityFrames > 0) "${d.streamBufferCapacityFrames} 帧" else "—"
    return listOf(
        MetricEntry("性能模式", performance),
        MetricEntry("Burst", burst),
        MetricEntry("设备缓冲容量", capacity),
    )
}

/** 连接：链路 + 会话信息（ID / 时长 / 数据源）。
 *  fullRow 标记"数据源"整行显示（IPv6 地址很长，两列布局会被截断）。 */
private data class MetricEntry(
    val label: String,
    val value: String,
    val fullRow: Boolean = false,
)

/** 连接（会话上下文）：链路/会话 ID → 时长/ACK → 数据源单行。 */
private fun qualityMetrics(
    d: AquaDiagnostics,
    f: AquaConnectResult,
    durationMs: Long?,
): List<MetricEntry> = listOf(
    MetricEntry("链路", if (d.helloFailed) "中断" else if (d.helloAckMisses > 0) "波动" else "正常"),
    MetricEntry("会话 ID", String.format(Locale.US, "%08x", f.sessionId)),
    MetricEntry("时长", durationMs?.let { formatDuration(it) } ?: "—"),
    MetricEntry("ACK", d.helloAckCount.f0()),
    MetricEntry(
        "数据源",
        if (f.learnedUdpAddress.isNotEmpty()) {
            formatHostPort(f.learnedUdpAddress, f.learnedUdpPort)
        } else {
            formatHostPort(f.advertisedUdpAddress, f.advertisedUdpPort)
        },
        fullRow = true,
    ),
)

/** 时长 mm:ss（≥1h 为 h:mm:ss）。 */
private fun formatDuration(ms: Long): String {
    val totalSec = ms / 1000
    val h = totalSec / 3600
    val m = totalSec % 3600 / 60
    val s = totalSec % 60
    return if (h > 0) {
        String.format(Locale.US, "%d:%02d:%02d", h, m, s)
    } else {
        String.format(Locale.US, "%02d:%02d", m, s)
    }
}

/** 传输（数据面收发）：收/发包计数 → 收/发流量 → 发送侧异常。 */
private fun transportMetrics(d: AquaDiagnostics): List<MetricEntry> = listOf(
    MetricEntry("收包", d.audioFramesAccepted.f0()),
    MetricEntry("收包总数", d.rxPackets.f0()),
    MetricEntry("发包", d.txPackets.f0()),
    MetricEntry("上行流量", d.txBytes.fBytes()),
    MetricEntry("下行流量", d.rxBytes.fBytes()),
    MetricEntry("发送丢弃", d.txDropped.f0()),
    MetricEntry("发送失败", d.txEnqueueFailures.f0()),
)

/** 缓冲（状态量 → 异常事件 → 机械值）：占用/水位 → 事件对 → 槽级统计。 */
private fun bufferMetrics(d: AquaDiagnostics): List<MetricEntry> = listOf(
    MetricEntry("占用", "${d.jbUsedSlots}/${d.jbCapacitySlots}"),
    MetricEntry("水位", String.format(Locale.US, "%.0f%%", d.jbWaterLevel * 100)),
    MetricEntry("补静音", d.jbFillEpisodes.f0()),
    MetricEntry("跳帧", d.jbDropEpisodes.f0()),
    MetricEntry("重锚定", d.jbReanchorCount.f0()),
    MetricEntry("迟到丢弃", d.jbPushRejectedLate.f0()),
    MetricEntry("拒收总数", d.jbPushRejected.f0()),
    MetricEntry("填充槽数", d.jbFillCorrectedSlots.f0()),
    MetricEntry("跳过槽数", d.jbDropSkippedSlots.f0()),
)

/** 播放消费（听感）：拉取节奏 → 静音帧 → 静音占比。 */
private fun playbackMetrics(d: AquaDiagnostics): List<MetricEntry> = listOf(
    MetricEntry("拉取", d.playbackPullCalls.f0()),
    MetricEntry("播放帧", d.playbackPullFrames.f0()),
    MetricEntry("静音帧", d.playbackPullSilenceFrames.f0()),
    MetricEntry("静音占比", String.format(Locale.US, "%.1f%%", d.silenceRatio * 100)),
)

/** 一组指标卡：图标 + 标题 + 两列 label/value 网格 + 可选占用进度条。
 *  fullRow 项独占一行（长值如 IPv6 数据源地址不被两列布局截断）。
 *  布局规则：普通项按两列排；fullRow 项强制换行独占。 */
@Composable
private fun MetricGroupCard(
    title: String,
    icon: ImageVector,
    metrics: List<MetricEntry>,
    progress: Float? = null,
) {
    OutlinedCard(Modifier.fillMaxWidth()) {
        Column(
            Modifier.padding(16.dp),
            verticalArrangement = Arrangement.spacedBy(12.dp),
        ) {
            Row(
                horizontalArrangement = Arrangement.spacedBy(8.dp),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Icon(
                    icon,
                    contentDescription = null,
                    tint = MaterialTheme.colorScheme.primary,
                )
                Text(
                    title,
                    style = MaterialTheme.typography.titleSmall,
                    fontWeight = FontWeight.Medium,
                )
                if (progress != null) {
                    Spacer(Modifier.weight(1f))
                    Text(
                        "占用 ${(progress * 100).roundToInt()}%",
                        style = MaterialTheme.typography.labelMedium,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                }
            }
            // 分行：fullRow 项独占一行，普通项两列配对；不足两列补空位。
            var index = 0
            while (index < metrics.size) {
                val entry = metrics[index]
                if (entry.fullRow) {
                    MetricCell(entry)
                    index++
                } else {
                    val next = metrics.getOrNull(index + 1)
                    if (next != null && !next.fullRow) {
                        Row(
                            Modifier.fillMaxWidth(),
                            horizontalArrangement = Arrangement.spacedBy(8.dp),
                        ) {
                            MetricCell(entry, Modifier.weight(1f))
                            MetricCell(next, Modifier.weight(1f))
                        }
                        index += 2
                    } else {
                        Row(
                            Modifier.fillMaxWidth(),
                            horizontalArrangement = Arrangement.spacedBy(8.dp),
                        ) {
                            MetricCell(entry, Modifier.weight(1f))
                            Spacer(Modifier.weight(1f))
                        }
                        index++
                    }
                }
            }
            if (progress != null) {
                LinearProgressIndicator(
                    progress = { progress.coerceIn(0f, 1f) },
                    modifier = Modifier.fillMaxWidth(),
                )
            }
        }
    }
}

/** 单个指标单元：label（弱化） + value（标题字号）。 */
@Composable
private fun MetricCell(entry: MetricEntry, modifier: Modifier = Modifier) {
    Column(modifier) {
        Text(
            entry.label,
            style = MaterialTheme.typography.labelMedium,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
        Text(
            entry.value,
            style = MaterialTheme.typography.titleSmall,
            fontWeight = FontWeight.Medium,
        )
    }
}

private fun Long.f0(): String = String.format(Locale.US, "%d", this)

/** 字节数带单位：B / KB / MB / GB。 */
private fun Long.fBytes(): String = when {
    this >= 1L shl 30 -> String.format(Locale.US, "%.2f GB", this / (1L shl 30).toDouble())
    this >= 1L shl 20 -> String.format(Locale.US, "%.2f MB", this / (1L shl 20).toDouble())
    this >= 1L shl 10 -> String.format(Locale.US, "%.1f KB", this / (1L shl 10).toDouble())
    else -> String.format(Locale.US, "%d B", this)
}

@Preview(showBackground = true)
@Composable
private fun AquaScreenPreview() {
    AquaTheme {
        AquaScreen(remember { AquaController() })
    }
}
