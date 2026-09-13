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
                    label = "最低水位",
                    valueText = if (controller.minTargetSlots == 0) "默认 3 槽" else "${controller.minTargetSlots} 槽",
                    tip = "自动水位的最小值，决定延迟下限。\n" +
                            "调大：断流频繁的网络更稳定，延迟下限随之抬高。\n" +
                            "调小：延迟更低；低于设备单次取数所需时会因数据不足而卡顿。\n" +
                            "与「缓冲容量」按 1:2 联动：调整本项时容量自动跟随至不小于其 2 倍，" +
                            "否则会被容量的 2/3 上限截断而无法生效。\n" +
                            "0 = 默认 3 槽；非低延迟模式建议不小于 40 槽。",
                    value = controller.minTargetSlots.toFloat(),
                    range = 0f..100f,
                    enabled = controller.jbSlidersEnabled,
                    onValueChange = { controller.updateMinTargetSlots(it.roundToInt()) },
                )
                HorizontalDivider()
                ParamSlider(
                    label = "缓冲容量",
                    valueText = if (controller.jbCapacity == 0) "默认 30 槽" else "${controller.jbCapacity} 槽",
                    tip = "环形缓冲的槽数，同时决定自动水位的上限（水位最高为容量的 2/3）。\n" +
                            "1 槽 ≈ 3.65ms，30 槽 ≈ 109ms。低于 4 槽无法启动。\n" +
                            "调大：抖动与断流的吸收能力更强。\n" +
                            "调小：延迟更低，抗抖动能力下降。\n" +
                            "水位长期贴近上限且无法继续升高时，应调整本项而非「抖动敏感度」。\n" +
                            "反向联动：调小容量时，「最低水位」会被压到容量的一半以内。\n" +
                            "0 = 默认 30 槽；非低延迟模式建议不小于 80 槽。",
                    value = controller.jbCapacity.toFloat(),
                    range = 0f..400f,
                    enabled = controller.jbSlidersEnabled,
                    onValueChange = { controller.updateJbCapacity(it.toInt()) },
                )
                HorizontalDivider()
                ParamSlider(
                    label = "抖动敏感度",
                    valueText = if (controller.jitterGain == 0.0) "默认 5.0" else halfText(
                        controller.jitterGain
                    ),
                    tip = "抖动观测值到水位余量的放大系数：观测抖动越大，自动囤积越多。\n" +
                            "调大：抗抖动能力增强，延迟升高。\n" +
                            "调小：延迟降低；抖动较大的链路会出现欠载。\n" +
                            "0 = 默认 5.0，已覆盖常规 Wi-Fi 场景。",
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
                    label = "断流应对上限",
                    valueText = if (controller.stallPeakCapSlots == 0.0) {
                        "默认 8.0 槽"
                    } else {
                        "${halfText(controller.stallPeakCapSlots)} 槽"
                    },
                    tip = "断流（到达间隔突增）期间允许追加的水位上限，默认 8 槽约 30ms。\n" +
                            "调大：可覆盖更长的断流间隙，断流结束后延迟回落所需时间更长。\n" +
                            "调小：延迟回落更快，中等长度的断流可能转为欠载。\n" +
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
                    tip = "断流峰值的衰减速率，数值越小记忆越久。\n" +
                            "调小：断流稀疏的网络保持更高水位，大断流后延迟回落更慢。\n" +
                            "调大：延迟回落更快，两次断流之间水位可能下降过多而再次欠载。\n" +
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
                    tip = "判定为断流的到达间隔阈值，单位为包的时长；断流与普通抖动走不同的处理路径。\n" +
                            "通常无需调整：调低会把正常发包误判为断流，调高会使真实断流被按抖动处理。\n" +
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
                    tip = "每次欠载后抬升水位下限的步长，为闭环补偿项，累计上限 6 槽，无欠载时逐步回落。\n" +
                            "调大：丢包与抖动较大的链路恢复更快，延迟上限相应抬高。\n" +
                            "链路正常时该值恒为 0，不引入额外延迟。\n" +
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
                "自定义模式：改动于下次连接生效，不影响当前播放。"
            } else {
                "当前套用预设「${controller.jbPreset.label}」，下方参数由预设统一赋值。" +
                        "如需逐项微调，请开启「自定义配置」。改动于下次连接生效。"
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
 * 网络环境预设：滑块从左到右 = 延迟从小到大 / 网络从优良到恶劣。
 *
 * 选中态是**显式事实**（[AquaController.jbPreset]），不是"反推当前参数等于哪个
 * 预设"：反推会让说明文字随下面的滑块拖动来回变长变短，整页跟着跳。
 * 自定义开关打开时本滑块锁定（改下面的高级滑块）；关闭时下面的高级滑块锁定
 * （由本滑块整体赋值）。
 */
@Composable
private fun PresetCard(controller: AquaController) {
    val presets = JbPreset.entries
    val index = presets.indexOf(controller.jbPreset).coerceAtLeast(0)
    val selected = presets[index]
    OutlinedCard(Modifier.fillMaxWidth()) {
        Column(
            Modifier.padding(horizontal = 16.dp, vertical = 12.dp),
            verticalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            Slider(
                value = index.toFloat(),
                onValueChange = { v ->
                    val picked = presets[v.roundToInt().coerceIn(0, presets.lastIndex)]
                    if (picked != controller.jbPreset) {
                        controller.applyJbPreset(picked)
                    }
                },
                valueRange = 0f..(presets.lastIndex).toFloat(),
                steps = presets.lastIndex - 1, // 档位数 - 2（首尾各算一档）
                enabled = !controller.jbCustom,
                modifier = Modifier.fillMaxWidth(),
            )
            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.SpaceBetween,
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Text(
                    "延迟更低",
                    style = MaterialTheme.typography.labelSmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
                Text(
                    selected.label,
                    style = MaterialTheme.typography.bodyMedium,
                    fontWeight = FontWeight.Medium,
                    color = MaterialTheme.colorScheme.primary,
                )
                Text(
                    "更抗网络",
                    style = MaterialTheme.typography.labelSmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
            // 与下面高级滑块同一款 tips 小块：一句话要点 + 完整说明。
            Surface(
                color = MaterialTheme.colorScheme.surfaceVariant.copy(alpha = 0.35f),
                shape = MaterialTheme.shapes.small,
                modifier = Modifier.fillMaxWidth(),
            ) {
                Column(Modifier.padding(horizontal = 10.dp, vertical = 8.dp)) {
                    Text(
                        selected.hint,
                        style = MaterialTheme.typography.bodySmall,
                        fontWeight = FontWeight.Medium,
                        color = MaterialTheme.colorScheme.onSurface,
                    )
                    Text(
                        selected.summary,
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                        modifier = Modifier.padding(top = 4.dp),
                    )
                }
            }
            HorizontalDivider()
            SettingSwitchInline(
                title = "自定义配置",
                subtitle = "使用自定义的微调选项，不使用预设",
                checked = controller.jbCustom,
                onCheckedChange = { controller.jbCustom = it },
            )
        }
    }
}

/** 紧凑开关行（与设置页同一语义，只是排版更紧）。 */
@Composable
private fun SettingSwitchInline(
    title: String,
    subtitle: String,
    checked: Boolean,
    onCheckedChange: (Boolean) -> Unit,
) {
    Row(
        modifier = Modifier.fillMaxWidth(),
        horizontalArrangement = Arrangement.SpaceBetween,
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Column(Modifier.weight(1f)) {
            Text(title, style = MaterialTheme.typography.bodyMedium)
            Text(
                subtitle,
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
        androidx.compose.material3.Switch(
            checked = checked,
            onCheckedChange = onCheckedChange,
        )
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
