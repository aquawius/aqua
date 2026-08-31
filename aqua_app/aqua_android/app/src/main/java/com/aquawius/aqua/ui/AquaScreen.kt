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
            OutlinedTextField(
                value = controller.serverIp,
                onValueChange = { controller.serverIp = it },
                label = { Text("服务器地址") },
                singleLine = true,
                enabled = !controller.isRunning,
                leadingIcon = { Icon(Icons.Filled.Dns, contentDescription = null) },
                modifier = Modifier.fillMaxWidth(),
            )
            OutlinedTextField(
                value = controller.rpcPort,
                onValueChange = { controller.rpcPort = it.filter { c -> c.isDigit() } },
                label = { Text("gRPC 端口") },
                singleLine = true,
                enabled = !controller.isRunning,
                leadingIcon = { Icon(Icons.Filled.Tag, contentDescription = null) },
                keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number),
                modifier = Modifier.fillMaxWidth(),
            )

            MetricsSection(
                controller.state,
                controller.connecting,
                controller.diagnostics,
                controller.connectResult,
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

/** 状态横幅：按状态着色的 tonal surface + 图标 + 错误详情。
 *  连接中（用户点击或自动重连）显示进行时反馈；首次连接未成功即停止
 *  （connectionFailed）视为"连接失败"，而非"已停止"。 */
@Composable
private fun StatusBanner(controller: AquaController) {
    val scheme = MaterialTheme.colorScheme
    val state = controller.state
    val style = when {
        state == AquaRuntimeState.RUNNING || state == AquaRuntimeState.DEGRADED -> StatusStyle(
            scheme.primaryContainer, scheme.onPrimaryContainer, Icons.Filled.CheckCircle,
        )
        controller.connecting -> StatusStyle(
            scheme.tertiaryContainer, scheme.onTertiaryContainer, Icons.Filled.Autorenew,
        )
        state == AquaRuntimeState.STOPPED && controller.connectionFailed -> StatusStyle(
            scheme.errorContainer, scheme.onErrorContainer, Icons.Filled.Error,
        )
        else -> StatusStyle(
            scheme.secondaryContainer, scheme.onSecondaryContainer, Icons.Filled.Info,
        )
    }
    val label = when {
        controller.connecting && controller.autoReconnectActive -> "自动重连中"
        controller.connecting -> "连接中"
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

/** 主操作按钮三态：连接中（禁用+进行时）/ 运行中（断开）/ 空闲（连接）。 */
@Composable
private fun ConnectButton(controller: AquaController) {
    when {
        controller.connecting -> {
            Button(
                onClick = { },
                enabled = false,
                modifier = Modifier
                    .fillMaxWidth()
                    .height(52.dp),
            ) {
                Icon(Icons.Filled.Autorenew, contentDescription = null)
                Spacer(Modifier.width(8.dp))
                Text("连接中…")
            }
        }
        controller.isRunning -> {
            FilledTonalButton(
                onClick = { controller.disconnect() },
                modifier = Modifier
                    .fillMaxWidth()
                    .height(52.dp),
            ) {
                Icon(Icons.Filled.Stop, contentDescription = null)
                Spacer(Modifier.width(8.dp))
                Text("断开连接")
            }
        }
        else -> {
            Button(
                onClick = { controller.connect() },
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
}

/** 核心指标（面向用户精选）：音频契约卡恒显（未连接时占位 "—"，同老版）；
 *  连接质量/缓冲水位仅在已连接时渲染，未连接显示引导/进行时占位。 */
@Composable
private fun MetricsSection(
    state: AquaRuntimeState,
    connecting: Boolean,
    d: AquaDiagnostics?,
    format: AquaConnectResult?,
) {
    MetricGroupCard("音频", Icons.Filled.GraphicEq, audioMetrics(format))

    val connected = state == AquaRuntimeState.RUNNING || state == AquaRuntimeState.DEGRADED
    if (!connected) {
        if (connecting) {
            PlaceholderCard("连接中…")
        } else {
            PlaceholderCard("连接后此处显示连接质量与缓冲水位")
        }
        return
    }
    if (d != null) {
        MetricGroupCard("连接质量", Icons.Filled.NetworkCheck, qualityMetrics(d))
        MetricGroupCard(
            title = "缓冲水位",
            icon = Icons.Filled.Storage,
            metrics = bufferMetrics(d),
            progress = d.jbWaterLevel.toFloat(),
        )
    } else {
        PlaceholderCard("正在收集数据…")
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

private fun audioMetrics(f: AquaConnectResult?): List<Pair<String, String>> {
    if (f == null) {
        return listOf(
            "采样率" to "—",
            "声道" to "—",
            "编码" to "—",
            "位深" to "—",
            "码率" to "—",
        )
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
        "采样率" to sampleRateText,
        "声道" to channelsText,
        "编码" to f.encoding.label,
        "位深" to if (f.encoding.bitsPerSample > 0) "${f.encoding.bitsPerSample} bit" else "—",
        "码率" to if (f.bitRateKbps > 0) "${f.bitRateKbps} kbps" else "—",
        "帧长 F" to if (f.frameCount > 0) {
            String.format(Locale.US, "%.1f ms", f.frameCount * 1000.0 / f.sampleRate)
        } else {
            "—"
        },
    )
}

/** 连接质量（用户视角）：链路健康 + 流量 + 静音占比（可听的卡顿感）。
 *  ACK 显示累计计数（age 是 0~间隔 的锯齿值，跳变无观察价值）。
 *  丢包/畸形包反映 Wi-Fi 质量；错发/错会话反映网络环境异常。 */
private fun qualityMetrics(d: AquaDiagnostics): List<Pair<String, String>> = listOf(
    "链路" to if (d.helloFailed) "中断" else if (d.helloAckMisses > 0) "波动" else "正常",
    "静音占比" to String.format(Locale.US, "%.1f%%", d.silenceRatio * 100),
    "收包" to d.audioFramesAccepted.f0(),
    "ACK" to d.helloAckCount.f0(),
    "流量" to d.rxBytes.fBytes(),
    "错包" to (d.malformedDatagrams + d.unexpectedSenderDatagrams).f0(),
)

/** 缓冲水位（用户视角）：占用量 + 补静音/跳帧次数（听感异常的累计证据）。 */
private fun bufferMetrics(d: AquaDiagnostics): List<Pair<String, String>> = listOf(
    "占用" to "${d.jbUsedSlots}/${d.jbCapacitySlots}",
    "水位" to String.format(Locale.US, "%.0f%%", d.jbWaterLevel * 100),
    "补静音" to d.jbFillEpisodes.f0(),
    "跳帧" to d.jbDropEpisodes.f0(),
    "重锚定" to d.jbReanchorCount.f0(),
    "迟到丢弃" to d.jbPushRejectedLate.f0(),
)

/** 一组指标卡：图标 + 标题 + 两列 label/value 网格 + 可选占用进度条。 */
@Composable
private fun MetricGroupCard(
    title: String,
    icon: ImageVector,
    metrics: List<Pair<String, String>>,
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
            metrics.chunked(2).forEach { row ->
                Row(
                    Modifier.fillMaxWidth(),
                    horizontalArrangement = Arrangement.spacedBy(8.dp),
                ) {
                    row.forEach { (label, value) ->
                        Column(Modifier.weight(1f)) {
                            Text(
                                label,
                                style = MaterialTheme.typography.labelMedium,
                                color = MaterialTheme.colorScheme.onSurfaceVariant,
                            )
                            Text(
                                value,
                                style = MaterialTheme.typography.titleSmall,
                                fontWeight = FontWeight.Medium,
                            )
                        }
                    }
                    if (row.size == 1) Spacer(Modifier.weight(1f))
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
