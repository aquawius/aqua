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
import androidx.compose.foundation.clickable
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.ArrowDropDown
import androidx.compose.material.icons.filled.Person
import androidx.compose.material.icons.filled.Refresh
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
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
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.tooling.preview.Preview
import androidx.compose.ui.unit.dp
import com.aquawius.aqua.AquaController
import com.aquawius.aqua.ui.theme.AquaTheme

/** 高级：对齐 CLI 的可调参数 + 日志。实时指标在主页；完整诊断走 logcat（tag: aqua）。 */
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
                        valueText = if (controller.jitterBufferSlots == 0) "默认" else "${controller.jitterBufferSlots} 槽",
                        hint = "容量（槽），0 = 30",
                        value = controller.jitterBufferSlots.toFloat(),
                        range = 0f..120f,
                        onValueChange = {
                            // 1~3 是 core 非法值（MIN=4）：拖动时吸附到合法档位，
                            // 兜底校验仍在 connect() 前置（防持久化残留旧非法值）。
                            controller.jitterBufferSlots = when (val n = it.toInt()) {
                                in 1..3 -> 4
                                else -> n
                            }
                        },
                    )
                HorizontalDivider()
                ParamSlider(
                        label = "HELLO 间隔",
                        valueText = if (controller.helloIntervalMs == 0) "默认" else "${controller.helloIntervalMs} ms",
                        hint = "保活间隔，须远小于 5s 超时",
                        value = controller.helloIntervalMs.toFloat(),
                        range = 0f..2000f,
                        onValueChange = { controller.helloIntervalMs = it.toInt() },
                    )
                HorizontalDivider(Modifier.padding(top = 4.dp))
                OutlinedTextField(
                    value = controller.clientName,
                    onValueChange = { controller.clientName = it },
                    label = { Text("名称") },
                    singleLine = true,
                    leadingIcon = { Icon(Icons.Filled.Person, contentDescription = null) },
                    modifier = Modifier
                        .fillMaxWidth()
                        .padding(vertical = 12.dp),
                )
                OutlinedTextField(
                    value = controller.forceUdpPort,
                    onValueChange = { controller.forceUdpPort = it.filter { c -> c.isDigit() } },
                    label = { Text("UDP 端口覆盖") },
                    singleLine = true,
                    keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number),
                    modifier = Modifier
                        .fillMaxWidth()
                        .padding(vertical = 12.dp),
                )
                LogLevelRow(controller)
            }
        }

        Text(
            "连接成功后自动保存",
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

        // ---- 日志 ----
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

/** 日志级别行（CLI --log-level）：点击展开下拉，即时生效于下一次连接。 */
@Composable
private fun LogLevelRow(controller: AquaController) {
    var expanded by remember { mutableStateOf(false) }
    val levels = listOf(
        -1 to "默认",
        0 to "Trace",
        1 to "Debug",
        2 to "Info",
        3 to "Warn",
        4 to "Error",
        5 to "Fatal",
    )
    val current = levels.firstOrNull { it.first == controller.logLevel }?.second ?: "默认"

    Row(
        modifier = Modifier
            .fillMaxWidth()
            .clickable { expanded = true }
            .padding(vertical = 14.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Text("日志级别", style = MaterialTheme.typography.bodyMedium)
        Spacer(Modifier.weight(1f))
        Text(
            current,
            style = MaterialTheme.typography.bodyMedium,
            fontWeight = FontWeight.Medium,
            color = MaterialTheme.colorScheme.primary,
        )
        Icon(Icons.Filled.ArrowDropDown, contentDescription = null)
        DropdownMenu(expanded = expanded, onDismissRequest = { expanded = false }) {
            levels.forEach { (code, name) ->
                DropdownMenuItem(
                    text = { Text(name) },
                    onClick = {
                        controller.logLevel = code
                        expanded = false
                    },
                )
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
