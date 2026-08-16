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
import com.aquawius.aqua.ui.theme.AquaTheme

/** 高级：高级参数卡（滑块 + Aqua 名称，连接成功后自动保存）；日志随内容滚动，仅分隔线隔开。 */
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
                        label = "JitterBuffer 目标延迟",
                        valueText = "${controller.jitterLatencyMs} ms" +
                            (if (controller.jitterLatencyMs == 0) "（默认 30）" else ""),
                        hint = "自适应下限（越大抗抖动越强，但延迟越高）",
                        value = controller.jitterLatencyMs.toFloat(),
                        range = 0f..200f,
                        onValueChange = { controller.jitterLatencyMs = it.toInt() },
                    )
                HorizontalDivider()
                ParamSlider(
                        label = "自适应延迟上限",
                        valueText = if (controller.jitterMaxLatencyMs == 0) "关闭"
                            else "${controller.jitterMaxLatencyMs} ms",
                        hint = "网络差时目标延迟最高自动升到该值；需大于目标延迟，0 = 关闭",
                        value = controller.jitterMaxLatencyMs.toFloat(),
                        range = 0f..300f,
                        onValueChange = { controller.jitterMaxLatencyMs = it.toInt() },
                    )
                HorizontalDivider()
                ParamSlider(
                        label = "漂移 late 阈值",
                        valueText = "${controller.driftThreshold} 包" +
                            (if (controller.driftThreshold == 0) "（默认 15）" else ""),
                        hint = "连续迟到包达到该值时重置缓冲，越大越容忍时钟漂移",
                        value = controller.driftThreshold.toFloat(),
                        range = 0f..100f,
                        onValueChange = { controller.driftThreshold = it.toInt() },
                    )
                HorizontalDivider()
                ParamSlider(
                        label = "播放缓冲",
                        valueText = "${controller.playbackBufferKb} KB" +
                            (if (controller.playbackBufferKb == 0) "（默认 16）" else ""),
                        hint = "越大抗欠载越强，但起播延迟越高",
                        value = controller.playbackBufferKb.toFloat(),
                        range = 0f..1024f,
                        onValueChange = { controller.playbackBufferKb = it.toInt() },
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

        // 日志区：不再固定底部，与高级参数之间仅一条分隔线。
        HorizontalDivider()
        SectionHeader("日志")
        LogBox(controller)
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

@Preview(showBackground = true)
@Composable
private fun AdvancedScreenPreview() {
    AquaTheme {
        AdvancedScreen(remember { AquaController() })
    }
}
