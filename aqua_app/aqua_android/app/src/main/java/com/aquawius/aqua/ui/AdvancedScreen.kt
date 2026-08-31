package com.aquawius.aqua.ui

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Person
import androidx.compose.material.icons.filled.Refresh
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedCard
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Slider
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.tooling.preview.Preview
import androidx.compose.ui.unit.dp
import com.aquawius.aqua.AquaController
import com.aquawius.aqua.AquaDiagnostics
import com.aquawius.aqua.ui.theme.AquaTheme
import java.util.Locale

/** 高级：参数卡（抖动槽数 / HELLO 间隔 + Aqua 名称）+ 开发者诊断 + 日志。
 *  诊断为全量展示（面向调试），主页面只保留用户指标。 */
@Composable
fun AdvancedScreen(controller: AquaController, modifier: Modifier = Modifier) {
    Column(
        modifier = modifier
            .fillMaxSize()
            .verticalScroll(rememberScrollState())
            .padding(16.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        SectionHeader("高级参数")

        OutlinedCard(Modifier.fillMaxWidth()) {
            Column(Modifier.padding(horizontal = 16.dp, vertical = 4.dp)) {
                ParamSlider(
                        label = "抖动缓冲",
                        valueText = if (controller.jitterBufferSlots == 0) {
                            "默认（30 槽）"
                        } else {
                            "${controller.jitterBufferSlots} 槽"
                        },
                        hint = "JitterBuffer 容量（slot 数，每 slot 一帧）；抗网络抖动的缓冲垫",
                        value = controller.jitterBufferSlots.toFloat(),
                        range = 0f..120f,
                        onValueChange = { controller.jitterBufferSlots = it.toInt() },
                    )
                HorizontalDivider()
                ParamSlider(
                        label = "HELLO 间隔",
                        valueText = if (controller.helloIntervalMs == 0) {
                            "默认（1000 ms）"
                        } else {
                            "${controller.helloIntervalMs} ms"
                        },
                        hint = "UDP 保活间隔；须远小于服务端 5s 会话超时",
                        value = controller.helloIntervalMs.toFloat(),
                        range = 0f..2000f,
                        onValueChange = { controller.helloIntervalMs = it.toInt() },
                    )
                HorizontalDivider(Modifier.padding(top = 4.dp))
                OutlinedTextField(
                    value = controller.clientName,
                    onValueChange = { controller.clientName = it },
                    label = { Text("Aqua 名称（服务端显示的客户端标识）") },
                    singleLine = true,
                    leadingIcon = { Icon(Icons.Filled.Person, contentDescription = null) },
                    modifier = Modifier
                        .fillMaxWidth()
                        .padding(vertical = 12.dp),
                )
            }
        }

        Text(
            "参数在成功连接后自动保存，下次打开时恢复。",
            style = MaterialTheme.typography.labelMedium,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )

        OutlinedButton(
            onClick = { controller.restoreDefaults() },
            modifier = Modifier.fillMaxWidth(),
        ) {
            Icon(Icons.Filled.Refresh, contentDescription = null)
            Spacer(Modifier.width(8.dp))
            Text("恢复默认值")
        }

        // ---- 开发者诊断（全量）----
        HorizontalDivider()
        SectionHeader("诊断")
        controller.diagnostics?.let { DiagnosticsCard(it) } ?: Text(
            "连接后此处显示完整诊断。",
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )

        // ---- 日志 ----
        HorizontalDivider()
        SectionHeader("日志")
        LogBox(controller)
    }
}

/** 全量诊断卡：net / jitter buffer / playback 三组计数，等宽字体键值对。
 *  面向开发调试；字段语义见 AquaDiagnostics 注释。 */
@Composable
private fun DiagnosticsCard(d: AquaDiagnostics) {
    OutlinedCard(Modifier.fillMaxWidth()) {
        Column(
            Modifier.padding(12.dp),
            verticalArrangement = Arrangement.spacedBy(6.dp),
        ) {
            DiagRow("state", d.state.label)
            DiagRow("audio_error", d.lastAudioError.label)
            DiagRow("playback_running", d.playbackRunning.toString())

            HorizontalDivider()
            DiagGroupTitle("net")
            DiagRow("rx", "${d.rxPackets} pk / ${d.rxBytes.fBytes()}")
            DiagRow("rx_errors", d.rxErrors.f0())
            DiagRow("tx", "${d.txPackets} pk / ${d.txBytes.fBytes()}")
            DiagRow("tx_err / dropped / enqfail", "${d.txErrors} / ${d.txDropped} / ${d.txEnqueueFailures}")
            DiagRow("tx_queue_depth", d.txQueueDepth.f0())
            DiagRow("hello ack / misses / age_ms", "${d.helloAckCount} / ${d.helloAckMisses} / ${d.helloAckAgeMs}")
            DiagRow("hello attempts / miss_events", "${d.helloSendAttempts} / ${d.helloAckMissEvents}")
            DiagRow("hello_failed", d.helloFailed.toString())
            DiagRow("audio_accepted", d.audioFramesAccepted.f0())
            DiagRow("malformed / unexpected / wrong_sess", "${d.malformedDatagrams} / ${d.unexpectedSenderDatagrams} / ${d.wrongSessionAcks}")
            DiagRow("payload_mismatch / non_audio", "${d.audioPayloadMismatches} / ${d.nonAudioDatagrams}")

            HorizontalDivider()
            DiagGroupTitle("jitter buffer")
            DiagRow("water / used / cap", String.format(Locale.US, "%.2f / %d / %d", d.jbWaterLevel, d.jbUsedSlots, d.jbCapacitySlots))
            DiagRow("push ok / reject", "${d.jbPushAccepted} / ${d.jbPushRejected}")
            DiagRow("push late / busy / invalid / sanity", "${d.jbPushRejectedLate} / ${d.jbPushRejectedSlotBusy} / ${d.jbPushRejectedInvalid} / ${d.jbPushRejectedSanity}")
            DiagRow("pull calls / frames / silence", "${d.jbPullCalls} / ${d.jbPullFrames} / ${d.jbPullSilenceFrames}")
            DiagRow("fill episodes / slots", "${d.jbFillEpisodes} / ${d.jbFillCorrectedSlots}")
            DiagRow("drop episodes / slots", "${d.jbDropEpisodes} / ${d.jbDropSkippedSlots}")
            DiagRow("reanchor cnt / req / cancel / sanity", "${d.jbReanchorCount} / ${d.jbReanchorRequests} / ${d.jbReanchorCancels} / ${d.jbReanchorSanityRejections}")
            DiagRow("reanchor last_seq", d.jbLastReanchorSequence.f0())

            HorizontalDivider()
            DiagGroupTitle("playback")
            DiagRow("pull calls / frames / silence", "${d.playbackPullCalls} / ${d.playbackPullFrames} / ${d.playbackPullSilenceFrames}")
        }
    }
}

@Composable
private fun DiagGroupTitle(text: String) {
    Text(
        text,
        style = MaterialTheme.typography.labelMedium,
        fontWeight = FontWeight.SemiBold,
        color = MaterialTheme.colorScheme.primary,
    )
}

@Composable
private fun DiagRow(key: String, value: String) {
    Row(Modifier.fillMaxWidth()) {
        Text(
            key,
            style = MaterialTheme.typography.labelSmall,
            fontFamily = FontFamily.Monospace,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
            modifier = Modifier.weight(0.55f),
        )
        Text(
            value,
            style = MaterialTheme.typography.labelSmall,
            fontFamily = FontFamily.Monospace,
            modifier = Modifier.weight(0.45f),
        )
    }
}

/** 日志框：等宽字体日志列表，自动滚动到底部（随内容滚动，不遮挡）。 */
@Composable
private fun LogBox(controller: AquaController) {
    OutlinedCard(
        modifier = Modifier
            .fillMaxWidth()
            .height(200.dp),
    ) {
        val listState = rememberLazyListState()
        LaunchedEffect(controller.log.size) {
            if (controller.log.isNotEmpty()) {
                listState.scrollToItem(controller.log.size - 1)
            }
        }
        if (controller.log.isEmpty()) {
            Text(
                "暂无日志",
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                modifier = Modifier
                    .padding(12.dp)
                    .align(Alignment.Start),
            )
        } else {
            LazyColumn(
                state = listState,
                modifier = Modifier
                    .fillMaxSize()
                    .padding(horizontal = 12.dp, vertical = 8.dp),
            ) {
                items(controller.log) { line ->
                    Text(
                        text = line,
                        style = MaterialTheme.typography.labelSmall,
                        fontFamily = FontFamily.Monospace,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                        modifier = Modifier
                            .fillMaxWidth()
                            .padding(vertical = 2.dp),
                    )
                }
            }
        }
    }
}

@Composable
private fun SectionHeader(text: String) {
    Text(
        text,
        style = MaterialTheme.typography.titleSmall,
        fontWeight = FontWeight.Medium,
        color = MaterialTheme.colorScheme.primary,
    )
}

/** 滑块行：参数名 + 当前值 + 滑块 + 说明注释。 */
@Composable
private fun ParamSlider(
    label: String,
    valueText: String,
    hint: String,
    value: Float,
    range: ClosedFloatingPointRange<Float>,
    onValueChange: (Float) -> Unit,
) {
    Column(Modifier.padding(vertical = 8.dp)) {
        Row(
            modifier = Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.SpaceBetween,
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Text(label, style = MaterialTheme.typography.bodyMedium)
            Text(
                valueText,
                style = MaterialTheme.typography.bodyMedium,
                fontWeight = FontWeight.Medium,
                color = MaterialTheme.colorScheme.primary,
            )
        }
        Slider(
            value = value,
            onValueChange = onValueChange,
            valueRange = range,
            modifier = Modifier.fillMaxWidth(),
        )
        Text(
            hint,
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
    }
}

private fun Long.f0(): String = String.format(Locale.US, "%d", this)

private fun Long.fBytes(): String = when {
    this >= 1L shl 30 -> String.format(Locale.US, "%.2f GB", this / (1L shl 30).toDouble())
    this >= 1L shl 20 -> String.format(Locale.US, "%.2f MB", this / (1L shl 20).toDouble())
    this >= 1L shl 10 -> String.format(Locale.US, "%.1f KB", this / (1L shl 10).toDouble())
    else -> String.format(Locale.US, "%d B", this)
}

@Preview(showBackground = true)
@Composable
private fun AdvancedScreenPreview() {
    AquaTheme {
        AdvancedScreen(remember { AquaController() })
    }
}
