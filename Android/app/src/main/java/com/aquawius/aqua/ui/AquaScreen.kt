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
import androidx.compose.material.icons.filled.Info
import androidx.compose.material.icons.filled.Insights
import androidx.compose.material.icons.filled.Memory
import androidx.compose.material.icons.filled.NetworkCheck
import androidx.compose.material.icons.filled.PlayArrow
import androidx.compose.material.icons.filled.Speed
import androidx.compose.material.icons.filled.Storage
import androidx.compose.material.icons.filled.Stop
import androidx.compose.material.icons.filled.Tag
import androidx.compose.material3.Button
import androidx.compose.material3.FilledTonalButton
import androidx.compose.material3.Icon
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
import com.aquawius.aqua.AquaClientState
import com.aquawius.aqua.AquaController
import com.aquawius.aqua.AquaDiagnostics
import com.aquawius.aqua.ui.theme.AquaTheme
import java.util.Locale

/** 首页：地址 + gRPC 端口 + 状态横幅 + 分组指标卡 + 连接按钮。 */
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

            StatusBanner(controller.state, controller.lastError)

            MetricsSection(controller.diagnostics)
        }

        ConnectButton(controller)
    }
}

private data class StatusStyle(
    val container: Color,
    val onContainer: Color,
    val icon: ImageVector,
    val label: String,
)

/** 状态横幅：按状态着色的 tonal surface + 图标 + 错误详情。 */
@Composable
private fun StatusBanner(state: AquaClientState, lastError: String) {
    val scheme = MaterialTheme.colorScheme
    val style = when (state) {
        AquaClientState.PLAYING -> StatusStyle(
            scheme.primaryContainer, scheme.onPrimaryContainer,
            Icons.Filled.CheckCircle, "播放中",
        )
        AquaClientState.CONNECTING -> StatusStyle(
            scheme.tertiaryContainer, scheme.onTertiaryContainer,
            Icons.Filled.Autorenew, "连接中",
        )
        AquaClientState.RECONNECTING -> StatusStyle(
            scheme.tertiaryContainer, scheme.onTertiaryContainer,
            Icons.Filled.Autorenew, "重连中",
        )
        AquaClientState.FAILED -> StatusStyle(
            scheme.errorContainer, scheme.onErrorContainer,
            Icons.Filled.Error, "连接失败",
        )
        AquaClientState.STOPPED -> StatusStyle(
            scheme.secondaryContainer, scheme.onSecondaryContainer,
            Icons.Filled.Info, "已停止",
        )
        AquaClientState.IDLE -> StatusStyle(
            scheme.secondaryContainer, scheme.onSecondaryContainer,
            Icons.Filled.Info, "未连接",
        )
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
                    style.label,
                    style = MaterialTheme.typography.titleSmall,
                    fontWeight = FontWeight.SemiBold,
                )
                if (lastError.isNotEmpty()) {
                    Text(
                        lastError,
                        style = MaterialTheme.typography.bodySmall,
                        maxLines = 2,
                        overflow = TextOverflow.Ellipsis,
                    )
                }
            }
        }
    }
}

/** 主操作按钮：连接（filled）/ 断开（tonal）。 */
@Composable
private fun ConnectButton(controller: AquaController) {
    if (controller.isRunning) {
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
    } else {
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

/** 分组指标：网络 / 抖动缓冲 / 播放缓冲 / 稳定性；无数据时展示占位卡。 */
@Composable
private fun MetricsSection(d: AquaDiagnostics?) {
    if (d == null) {
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
                    "连接并开始播放后，此处显示实时指标",
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
        }
        return
    }
    MetricGroupCard("网络", Icons.Filled.NetworkCheck, networkMetrics(d))
    MetricGroupCard("抖动缓冲（JB）", Icons.Filled.Storage, jitterMetrics(d))
    MetricGroupCard("播放缓冲（RB）", Icons.Filled.Memory, ringMetrics(d))
    MetricGroupCard("稳定性", Icons.Filled.Speed, stabilityMetrics(d))
}

/** 一组指标卡：图标 + 标题 + 两列 label/value 网格。 */
@Composable
private fun MetricGroupCard(title: String, icon: ImageVector, metrics: List<Pair<String, String>>) {
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
        }
    }
}

private fun networkMetrics(d: AquaDiagnostics): List<Pair<String, String>> = listOf(
    "RTT" to "${d.rttMs.f1()} ms",
    "抖动" to "${d.interarrivalJitterMs.f2()} ms",
    "丢包" to d.packetsLost.f0(),
    "重复" to d.duplicates.f0(),
    "迟到" to d.latePackets.f0(),
    "收包" to d.packetsReceived.f0(),
    "ACK" to d.recvHelloAcks.f0(),
    "音频字节" to d.recvAudioBytes.f0(),
)

private fun jitterMetrics(d: AquaDiagnostics): List<Pair<String, String>> = listOf(
    "包数" to d.jbCurrentPackets.f0(),
    "容量" to "${d.jbCapacityMs.f1()} ms",
    "当前" to "${d.jbCurrentMs.f1()} ms",
    "平均" to "${d.jbAvgMs.f1()} ms",
    "最小" to "${d.jbMinMs.f1()} ms",
    "最大" to "${d.jbMaxMs.f1()} ms",
)

private fun ringMetrics(d: AquaDiagnostics): List<Pair<String, String>> = listOf(
    "当前" to "${d.rbCurrentMs.f1()} ms",
    "平均" to "${d.rbAvgMs.f1()} ms",
    "最小" to "${d.rbMinMs.f1()} ms",
    "最大" to "${d.rbMaxMs.f1()} ms",
    "容量" to "${d.rbCapacityMs.f1()} ms",
)

private fun stabilityMetrics(d: AquaDiagnostics): List<Pair<String, String>> = listOf(
    "端到端" to "${d.endToEndMs.f1()} ms",
    "漂移" to "${d.driftPpm.f1()} ppm",
    "欠载" to d.underruns.f0(),
    "错过 deadline" to d.deadlineMisses.f0(),
    "斜率短" to d.shortSlopeSamplesPerS.f1(),
    "斜率长" to d.longSlopeSamplesPerS.f1(),
)

private fun Double.f0(): String = String.format(Locale.US, "%.0f", this)
private fun Double.f1(): String = String.format(Locale.US, "%.1f", this)
private fun Double.f2(): String = String.format(Locale.US, "%.2f", this)

@Preview(showBackground = true)
@Composable
private fun AquaScreenPreview() {
    AquaTheme {
        AquaScreen(remember { AquaController() })
    }
}
