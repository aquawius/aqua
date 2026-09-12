package com.aquawius.aqua.ui

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.text.KeyboardOptions
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
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.tooling.preview.Preview
import androidx.compose.ui.unit.dp
import com.aquawius.aqua.AquaController
import com.aquawius.aqua.JbPreset
import com.aquawius.aqua.ui.theme.AquaTheme
import java.util.Locale
import kotlin.math.roundToInt

/**
 * 高级：对齐 CLI 的可调参数（实时指标在主页；应用日志在设置页；native 日志走 logcat，tag: aqua）。
 *
 * 分三块，顺序即推荐的使用顺序：
 *  1. 网络环境预设 —— 按链路类型一键套一组参数（值来自设计文档 §9.1 的推荐起点）；
 *  2. 抖动缓冲 —— JB 自适应旋钮，每个都带说明（调整了什么 + 调大调小的代价）；
 *  3. 连接 —— 与 JB 无关的连接参数。
 */
@Composable
fun AdvancedScreen(controller: AquaController, modifier: Modifier = Modifier) {
    Column(
        modifier = modifier
            .fillMaxSize()
            .verticalScroll(rememberScrollState())
            .padding(16.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        SectionHeader("网络环境预设")
        PresetCard(controller)

        SectionHeader("抖动缓冲")
        OutlinedCard(Modifier.fillMaxWidth()) {
            Column(Modifier.padding(horizontal = 16.dp, vertical = 4.dp)) {
                ParamSlider(
                    label = "缓冲容量",
                    valueText = if (controller.jbCapacity == 0) "默认 30 槽" else "${controller.jbCapacity} 槽",
                    tip = "最多囤多少声音。1 槽 ≈ 3.75ms，30 槽 ≈ 112ms。\n" +
                        "调大：更能扛网络抖动和断流，代价是延迟变高。\n" +
                        "调小：延迟更低，但网络一抖就容易卡。低于 4 槽无法启动。\n" +
                        "0 = 默认 30 槽。\n" +
                        "重要：自动水位（target）**最高只能到容量的 2/3**（默认 30 槽 → 水位最多 " +
                        "20 槽 ≈ 75ms）。所以你觉得水位上不去、怎么调都停在 20 出头，先调的是这里，" +
                        "不是下面的抖动敏感度。\n" +
                        "非低延迟模式下音频设备每次取数可达数千帧（实测 3844 帧），默认容量喂不饱，" +
                        "建议调到 100 槽以上。",
                    value = controller.jbCapacity.toFloat(),
                    range = 0f..400f,
                    enabled = controller.jbSlidersEnabled,
                    onValueChange = {
                        // 1~3 是 core 非法值（MIN=4）：拖动时吸附到合法档位，
                        // 兜底校验仍在 connect() 前置（防持久化残留旧非法值）。
                        controller.markJbCustom()
                        controller.jbCapacity = when (val n = it.toInt()) {
                            in 1..3 -> 4
                            else -> n
                        }
                    },
                )
                HorizontalDivider()
                ParamSlider(
                    label = "抖动敏感度",
                    valueText = if (controller.jitterGain == 0.0) "默认 5.0" else halfText(controller.jitterGain),
                    tip = "网络抖动的放大倍数：网络越抖，自动囤得越多。\n" +
                        "调大：更抗抖、卡顿更少，延迟更高。\n" +
                        "调小：延迟更低，但网络一抖就容易卡；干净网络会一路压到最低水位。\n" +
                        "0 = 默认 5.0（已覆盖常规 Wi-Fi）。只在明显卡顿或明显嫌延迟大时才动它。",
                    value = controller.jitterGain.toFloat(),
                    range = 0f..20f,
                    enabled = controller.jbSlidersEnabled,
                    onValueChange = {
                        controller.markJbCustom()
                        controller.jitterGain = snapHalf(it)
                    },
                )
                HorizontalDivider()
                ParamSlider(
                    label = "最低水位",
                    valueText = if (controller.minTargetSlots == 0) "默认 3 槽" else "${controller.minTargetSlots} 槽",
                    tip = "最少囤多少声音。播放设备每次会固定取走一坨数据，压到它下面必然喂不饱，所以再调也不会更低。\n" +
                        "调大：断流多的网络（公网 / 弱网）更稳，代价是延迟下限变高。\n" +
                        "同样受「容量的 2/3」限制：默认 30 槽时这里最多只能到 20 槽，想抬更高请先把" +
                        "「缓冲容量」调大。\n" +
                        "0 = 默认 3 槽。",
                    value = controller.minTargetSlots.toFloat(),
                    range = 0f..100f,
                    enabled = controller.jbSlidersEnabled,
                    onValueChange = {
                        controller.markJbCustom()
                        controller.minTargetSlots = it.roundToInt()
                    },
                )
                HorizontalDivider()
                ParamSlider(
                    label = "断流应对上限",
                    valueText = if (controller.stallPeakCapSlots == 0.0) {
                        "默认 8.0 槽"
                    } else {
                        "${halfText(controller.stallPeakCapSlots)} 槽"
                    },
                    tip = "网络短暂停顿时，最多额外囤多少来顶过去（默认 8 槽 ≈ 30ms）。\n" +
                        "调大：更长的停顿也能挺过去，代价是停顿之后延迟要过更久才降回来。\n" +
                        "调小：延迟回落更快，但中等长度的停顿会开始卡。\n" +
                        "0 = 默认 8.0 槽。",
                    value = controller.stallPeakCapSlots.toFloat(),
                    range = 0f..40f,
                    enabled = controller.jbSlidersEnabled,
                    onValueChange = {
                        controller.markJbCustom()
                        controller.stallPeakCapSlots = snapHalf(it)
                    },
                )
                HorizontalDivider()
                ParamSlider(
                    label = "断流记忆时长",
                    valueText = if (controller.stallPeakDecayMsPerSec == 0.0) {
                        "默认 10.0 ms/s"
                    } else {
                        "${halfText(controller.stallPeakDecayMsPerSec)} ms/s"
                    },
                    tip = "上一次停顿的教训记多久（数值越小记得越久）。\n" +
                        "调小：断流很稀疏的网络（公网）更稳，代价是一次大停顿后延迟在高位挂更久。\n" +
                        "调大：忘得快、延迟回落爽快，但两次停顿之间可能掉得太低又卡一次。\n" +
                        "0 = 默认 10.0 ms/s。",
                    value = controller.stallPeakDecayMsPerSec.toFloat(),
                    range = 0f..30f,
                    enabled = controller.jbSlidersEnabled,
                    onValueChange = {
                        controller.markJbCustom()
                        controller.stallPeakDecayMsPerSec = snapHalf(it)
                    },
                )
                HorizontalDivider()
                ParamSlider(
                    label = "断流判定门槛",
                    valueText = if (controller.stallThresholdPackets == 0.0) {
                        "默认 5.0 个包"
                    } else {
                        "${halfText(controller.stallThresholdPackets)} 个包"
                    },
                    tip = "间隔超过多少个包的时长算「断流」，而不是普通抖动——两者走两套应对逻辑。\n" +
                        "一般不用动：调低会把正常发包误判成断流，调高会让真断流被当成抖动处理。\n" +
                        "0 = 默认 5.0 个包。",
                    value = controller.stallThresholdPackets.toFloat(),
                    range = 0f..12f,
                    enabled = controller.jbSlidersEnabled,
                    onValueChange = {
                        controller.markJbCustom()
                        controller.stallThresholdPackets = snapHalf(it)
                    },
                )
                HorizontalDivider()
                ParamSlider(
                    label = "卡顿后加固步长",
                    valueText = if (controller.underrunPenaltySlots == 0.0) {
                        "默认 1.0 槽"
                    } else {
                        "${halfText(controller.underrunPenaltySlots)} 槽"
                    },
                    tip = "每卡一次，自动把最低水位抬高多少（安全网；累计最多 6 槽，一段时间不卡再慢慢降回来）。\n" +
                        "调大：丢包多、抖动大的网络恢复得更快，代价是延迟被顶得更高。\n" +
                        "干净网络上它一直是 0，不增加任何延迟。\n" +
                        "0 = 默认 1.0 槽。",
                    value = controller.underrunPenaltySlots.toFloat(),
                    range = 0f..6f,
                    enabled = controller.jbSlidersEnabled,
                    onValueChange = {
                        controller.markJbCustom()
                        controller.underrunPenaltySlots = snapHalf(it)
                    },
                )
            }
        }
        Text(
            if (controller.jbSlidersEnabled) {
                "自定义模式：改动在下次连接时生效（不会打断当前播放）"
            } else {
                "当前套用的是「${controller.jbPreset.label}」预设，滑块已锁定。" +
                    "要逐项微调请点上面的「自定义」。改动在下次连接时生效。"
            },
            style = MaterialTheme.typography.labelMedium,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )

        SectionHeader("连接")
        OutlinedCard(Modifier.fillMaxWidth()) {
            Column(Modifier.padding(horizontal = 16.dp, vertical = 4.dp)) {
                ParamSlider(
                    label = "Heartbeat 间隔",
                    valueText = if (controller.heartbeatHandshakeIntervalMs == 0) "默认 1000 ms" else "${controller.heartbeatHandshakeIntervalMs} ms",
                    tip = "UDP 保活心跳间隔，需要小于服务器的会话超时时间。",
                    value = controller.heartbeatHandshakeIntervalMs.toFloat(),
                    range = 0f..2000f,
                    onValueChange = { controller.heartbeatHandshakeIntervalMs = it.toInt() },
                )
                HorizontalDivider(Modifier.padding(top = 4.dp))
                OutlinedTextField(
                    value = controller.clientName,
                    onValueChange = { controller.clientName = it },
                    label = { Text("名称") },
                    supportingText = { Text("服务端显示的客户端标识") },
                    singleLine = true,
                    leadingIcon = { Icon(Icons.Filled.Person, contentDescription = null) },
                    modifier = Modifier
                        .fillMaxWidth()
                        .padding(top = 12.dp, bottom = 4.dp),
                )
                OutlinedTextField(
                    value = controller.udpForcePort,
                    onValueChange = { controller.udpForcePort = it.filter { c -> c.isDigit() } },
                    label = { Text("UDP 端口覆盖") },
                    supportingText = { Text("覆盖服务器通告的UDP端口，用于NAT/端口映射场景") },
                    singleLine = true,
                    keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number),
                    modifier = Modifier
                        .fillMaxWidth()
                        .padding(top = 4.dp, bottom = 12.dp),
                )
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

        // ---- 应用事件日志（用户视角；系统级日志级别在设置页）----
        HorizontalDivider()
        LogBox(controller)
    }
}

/**
 * 网络环境预设卡：每个预设一个按钮（两列排布），下方给出当前选中预设的说明。
 *
 * 选中态是**显式事实**（[AquaController.jbPreset]），不是"反推当前参数等于哪个
 * 预设"：反推会让说明文字随滑块拖动来回变长变短，整页跟着跳。切到「自定义」
 * 之前滑块是锁着的，因此说明文字只会在**用户点预设**时变一次。
 */
@Composable
private fun PresetCard(controller: AquaController) {
    val selected = controller.jbPreset
    OutlinedCard(Modifier.fillMaxWidth()) {
        Column(
            Modifier.padding(16.dp),
            verticalArrangement = Arrangement.spacedBy(10.dp),
        ) {
            Text(
                "先按网络挑一组（值取自设计文档 §9.1 的推荐起点）",
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
            JbPreset.entries.chunked(2).forEach { row ->
                Row(
                    Modifier.fillMaxWidth(),
                    horizontalArrangement = Arrangement.spacedBy(8.dp),
                ) {
                    row.forEach { preset ->
                        OutlinedButton(
                            onClick = { controller.applyJbPreset(preset) },
                            modifier = Modifier.weight(1f),
                        ) {
                            Text(
                                if (preset == selected) "✓ ${preset.label}" else preset.label,
                                style = MaterialTheme.typography.labelMedium,
                                maxLines = 1,
                            )
                        }
                    }
                    // 奇数个时补空位，避免最后一行单个按钮被拉满整行。
                    if (row.size == 1) {
                        Spacer(Modifier.weight(1f))
                    }
                }
            }
            // 说明区预留固定高度：各预设文案长短不一，不预留会在切换预设时
            // 把下面的滑块整块推上推下。
            Column(Modifier.heightIn(min = 104.dp)) {
                Text(
                    selected.summary,
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
        }
    }
}

/** 滑块值吸附到 0.5 步进；0 保持可达（= "用 core 默认值"）。 */
private fun snapHalf(v: Float): Double = (v * 2f).roundToInt() / 2.0

/** 0.5 步进值的显示（整数不带小数点）。 */
private fun halfText(v: Double): String =
    if (v == v.roundToInt().toDouble()) String.format(Locale.US, "%d", v.toInt())
    else String.format(Locale.US, "%.1f", v)

/** 日志框：等宽字体日志列表，自动滚动到底部。 */
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

/** 滑块行：参数名 + 当前值 + 滑块 + 说明块（tip = 调什么 + 调大调小的代价）。
 *  [enabled] = false（套用预设中）时整行禁用：预设是一组配套值，逐项改会让它
 *  失去意义；要微调先切到「自定义」。 */
@Composable
private fun ParamSlider(
    label: String,
    valueText: String,
    tip: String,
    value: Float,
    range: ClosedFloatingPointRange<Float>,
    onValueChange: (Float) -> Unit,
    enabled: Boolean = true,
) {
    Column(Modifier.padding(vertical = 8.dp)) {
        Row(
            modifier = Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.SpaceBetween,
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Text(
                label,
                style = MaterialTheme.typography.bodyMedium,
                color = if (enabled) {
                    MaterialTheme.colorScheme.onSurface
                } else {
                    MaterialTheme.colorScheme.onSurfaceVariant
                },
            )
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
            enabled = enabled,
            modifier = Modifier.fillMaxWidth(),
        )
        // tip 用独立底色块呈现：与数值行区分开，扫描时更容易跳过或停留。
        Surface(
            color = MaterialTheme.colorScheme.surfaceVariant.copy(alpha = 0.35f),
            shape = MaterialTheme.shapes.small,
            modifier = Modifier
                .fillMaxWidth()
                .padding(top = 6.dp),
        ) {
            Text(
                tip,
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                modifier = Modifier.padding(horizontal = 10.dp, vertical = 8.dp),
            )
        }
    }
}

@Preview(showBackground = true)
@Composable
private fun AdvancedScreenPreview() {
    AquaTheme {
        AdvancedScreen(remember { AquaController() })
    }
}
