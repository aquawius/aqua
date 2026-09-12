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
                    label = "缓冲容量",
                    valueText = if (controller.jbCapacity == 0) "默认 30 槽" else "${controller.jbCapacity} 槽",
                    tip = "环形缓冲的槽数，是延迟 ↔ 抗抖动的主刻度。1 槽 ≈ 3.75ms（180 帧 @48kHz），" +
                        "30 槽 ≈ 112ms。\n" +
                        "调大：能吸收更大的抖动峰峰值，代价是稳态延迟变高；自适应 target 的上限是它的 " +
                        "2/3（上 1/3 留给抖动吸收），所以超出这部分的容量买的是余量而不是延迟。\n" +
                        "调小：延迟更低，但低于 4 槽会拒绝启动（五个水位带无法保持严格序）。\n" +
                        "0 = 默认 30 槽。低延迟模式下建议 30 槽；非低延迟模式下低于 100 槽可能让音频设备拿不到稳定缓冲。",
                    value = controller.jbCapacity.toFloat(),
                    range = 0f..400f,
                    onValueChange = {
                        // 1~3 是 core 非法值（MIN=4）：拖动时吸附到合法档位，
                        // 兜底校验仍在 connect() 前置（防持久化残留旧非法值）。
                        controller.jbCapacity = when (val n = it.toInt()) {
                            in 1..3 -> 4
                            else -> n
                        }
                    },
                )
                HorizontalDivider()
                ParamSlider(
                    label = "抖动增益 k",
                    valueText = if (controller.jitterGain == 0.0) "默认 5.0" else halfText(controller.jitterGain),
                    tip = "按平均抖动预留多少余量：target ≈ max(k × J, 断流峰值项)，J 是 RFC 3550 的到达抖动均值。" +
                        "本链路上 k 每 +1 约等于多 1.3 槽（≈5ms）延迟。\n" +
                        "调大：抗抖动更强、欠载更少，延迟更高；调到很大也不会失控——超出的部分被 2/3 容量上限接住。\n" +
                        "调小：延迟更低，干净网络会一路贴到几何地板（约 15ms）；抖动大的链路开始欠载。\n" +
                        "0 = 默认 5.0。（想要 k=0，即\"完全靠断流峰值项\"的极值实验，请用 CLI。）",
                    value = controller.jitterGain.toFloat(),
                    range = 0f..20f,
                    onValueChange = { controller.jitterGain = snapHalf(it) },
                )
                HorizontalDivider()
                ParamSlider(
                    label = "target 下限",
                    valueText = if (controller.minTargetSlots == 0) "默认 3 槽" else "${controller.minTargetSlots} 槽",
                    tip = "给自适应 target 划一条地板线。有效下限 = max(本值, 几何地板 + 1)，而几何地板" +
                        "（一次播放回调消耗的包数 + 1）无条件托底——所以这个值只能把最低延迟往上抬，" +
                        "压不到地板以下。\n" +
                        "调大：适合\"每次断流都要靠反馈慢慢补\"的链路（公网 / 弱网），用固定的一点延迟换稳定。\n" +
                        "调小到几何地板以下不会有效果（会被地板接住）。\n" +
                        "0 = 默认 3 槽。",
                    value = controller.minTargetSlots.toFloat(),
                    range = 0f..30f,
                    onValueChange = { controller.minTargetSlots = it.roundToInt() },
                )
                HorizontalDivider()
                ParamSlider(
                    label = "断流峰值上限",
                    valueText = if (controller.stallPeakCapSlots == 0.0) {
                        "默认 8.0 槽"
                    } else {
                        "${halfText(controller.stallPeakCapSlots)} 槽"
                    },
                    tip = "到达间隔超过「断流门」时，这段间隔不计入抖动均值 J，而是单独跟踪\"近期最坏间隙\"" +
                        "并换算成 target 抬升量；本值就是它能抬到的上限（默认 8 槽 ≈ 30ms）。\n" +
                        "调大：50~60ms 这种常态化的长间隙也能被目标水位挺过去，代价是一次事故会把延迟抬高更久。\n" +
                        "调小：延迟债务更轻，但 30ms 一档的断流会开始漏成欠载（改由欠载反馈与掩盖兜底）。\n" +
                        "0 = 默认 8.0 槽。（想要\"关闭峰值项\"的极值实验请用 CLI。）",
                    value = controller.stallPeakCapSlots.toFloat(),
                    range = 0f..40f,
                    onValueChange = { controller.stallPeakCapSlots = snapHalf(it) },
                )
                HorizontalDivider()
                ParamSlider(
                    label = "断流峰值衰减",
                    valueText = if (controller.stallPeakDecayMsPerSec == 0.0) {
                        "默认 10.0 ms/s"
                    } else {
                        "${halfText(controller.stallPeakDecayMsPerSec)} ms/s"
                    },
                    tip = "上面那个\"近期最坏间隙\"记多久：每次断流刷新为最大值，之后按本速率线性回落。\n" +
                        "调小：记得更久——稀疏断流（公网）之间也保持警惕，代价是一次大事故后 target 高位挂得更久。\n" +
                        "调大：忘得更快、延迟回落更爽快，但两次断流之间可能掉得太低、再次欠载。\n" +
                        "0 = 默认 10.0 ms/s。（想要\"峰值永久保持\"的极值实验请用 CLI。）",
                    value = controller.stallPeakDecayMsPerSec.toFloat(),
                    range = 0f..30f,
                    onValueChange = { controller.stallPeakDecayMsPerSec = snapHalf(it) },
                )
                HorizontalDivider()
                ParamSlider(
                    label = "断流门阈值",
                    valueText = if (controller.stallThresholdPackets == 0.0) {
                        "默认 5.0 个包周期"
                    } else {
                        "${halfText(controller.stallThresholdPackets)} 个包周期"
                    },
                    tip = "多长的到达间隔算\"断流\"而不是\"抖动\"，单位是包周期（本链路 1 个包周期 = 3.75ms）。" +
                        "超过它的间隔不进抖动均值 J，只计断流事件并刷新峰值跟踪。\n" +
                        "必须大于突发发包的串间间隔（约 2.7 个包周期），否则正常发包会被误判成断流。\n" +
                        "调大：只有更长的间隙才算断流，更多拥塞被当成抖动进 J，target 反应更迟钝。\n" +
                        "调小：更多间隙被剔出 J 交给峰值项处理，target 对短断流更敏感。\n" +
                        "0 = 默认 5.0。（想要\"关掉检测、完全回到 RFC 3550\"的对照请用 CLI。）",
                    value = controller.stallThresholdPackets.toFloat(),
                    range = 0f..12f,
                    onValueChange = { controller.stallThresholdPackets = snapHalf(it) },
                )
                HorizontalDivider()
                ParamSlider(
                    label = "欠载惩罚步长",
                    valueText = if (controller.underrunPenaltySlots == 0.0) {
                        "默认 1.0 槽"
                    } else {
                        "${halfText(controller.underrunPenaltySlots)} 槽"
                    },
                    tip = "预测项（抖动均值与断流峰值）都是统计量，覆盖不了随机抖动尾部与丢包；这一项是闭环安全网：" +
                        "每发生一次欠载，就把 target 的下限抬高这么多槽（累计上限 6 槽，之后按 0.5 槽/秒回落）。\n" +
                        "抬的是下限而不是叠加到余量上，所以抖动均值已经很高时不会重复放大；干净链路上它恒为 0，不影响延迟。\n" +
                        "调大：丢包多的链路恢复更快，代价是延迟被顶得更高。\n" +
                        "0 = 默认 1.0 槽。（想要\"关闭整条反馈闭环\"的对照请用 CLI。）",
                    value = controller.underrunPenaltySlots.toFloat(),
                    range = 0f..6f,
                    onValueChange = { controller.underrunPenaltySlots = snapHalf(it) },
                )
            }
        }
        Text(
            "抖动缓冲参数是连接属性：改动在下次连接时生效（不会打断当前播放）",
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
 * 网络环境预设卡：每个预设一个按钮（两列排布），下方给出当前生效组合的说明。
 *
 * "当前"是**实时算出来的**（把 7 个参数逐项与预设比对），不是"上次点了谁"——
 * 用户点完预设又拖了滑块，这里会立刻变成"自定义"。有线 LAN 与 Wi-Fi 的推荐值
 * 就是默认组合，因此默认状态下会同时匹配两个预设，此时并列显示。
 */
@Composable
private fun PresetCard(controller: AquaController) {
    val matched = JbPreset.entries.filter { presetMatches(it, controller) }
    OutlinedCard(Modifier.fillMaxWidth()) {
        Column(
            Modifier.padding(16.dp),
            verticalArrangement = Arrangement.spacedBy(10.dp),
        ) {
            Text(
                "按链路类型一键套用（值取自设计文档 §9.1 的推荐起点）",
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
                                if (matched.contains(preset)) "✓ ${preset.label}" else preset.label,
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
            val summary = when {
                matched.isEmpty() ->
                    "当前参数：自定义组合（与任何预设都不完全一致）。上面的说明可用于逐项对照。"
                matched.size == 1 -> "当前参数 =「${matched.first().label}」\n\n${matched.first().summary}"
                else ->
                    "当前参数 = 默认组合（${matched.joinToString(" / ") { it.label }}）\n\n" +
                        matched.first().summary
            }
            Text(
                summary,
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
            HorizontalDivider()
            Text(
                "注意：预设里的「容量」只按网络抖动定标。设备缓冲能否稳定还取决于「低延迟模式」——" +
                    "非低延迟模式下 AAudio 可能需要更大的容量，此时以「缓冲容量」滑块的说明为准。",
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
    }
}

/** 预设是否与当前参数完全一致（0 = core 默认值，故"默认组合"会同时匹配多个预设）。 */
private fun presetMatches(preset: JbPreset, c: AquaController): Boolean =
    preset.jbCapacity == c.jbCapacity &&
        preset.jitterGain == c.jitterGain &&
        preset.minTargetSlots == c.minTargetSlots &&
        preset.stallPeakCapSlots == c.stallPeakCapSlots &&
        preset.stallPeakDecayMsPerSec == c.stallPeakDecayMsPerSec &&
        preset.stallThresholdPackets == c.stallThresholdPackets &&
        preset.underrunPenaltySlots == c.underrunPenaltySlots

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

/** 滑块行：参数名 + 当前值 + 滑块 + 说明块（tip = 调整了什么 + 调大调小的影响）。 */
@Composable
private fun ParamSlider(
    label: String,
    valueText: String,
    tip: String,
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
