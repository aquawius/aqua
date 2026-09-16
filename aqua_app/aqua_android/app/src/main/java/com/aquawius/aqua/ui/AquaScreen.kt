package com.aquawius.aqua.ui

import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.core.tween
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.animation.slideInVertically
import androidx.compose.animation.slideOutVertically
import androidx.compose.foundation.clickable
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
import androidx.compose.material.icons.filled.AutoGraph
import androidx.compose.material.icons.filled.Autorenew
import androidx.compose.material.icons.filled.BluetoothAudio
import androidx.compose.material.icons.filled.Check
import androidx.compose.material.icons.filled.CheckCircle
import androidx.compose.material.icons.filled.Code
import androidx.compose.material.icons.filled.Dns
import androidx.compose.material.icons.filled.Error
import androidx.compose.material.icons.filled.GraphicEq
import androidx.compose.material.icons.filled.Headphones
import androidx.compose.material.icons.filled.Headset
import androidx.compose.material.icons.filled.Info
import androidx.compose.material.icons.filled.Insights
import androidx.compose.material.icons.filled.Memory
import androidx.compose.material.icons.filled.NetworkCheck
import androidx.compose.material.icons.filled.PlayArrow
import androidx.compose.material.icons.filled.SettingsEthernet
import androidx.compose.material.icons.filled.Speaker
import androidx.compose.material.icons.filled.SpeakerGroup
import androidx.compose.material.icons.filled.Stop
import androidx.compose.material.icons.filled.Storage
import androidx.compose.material.icons.filled.SwapHoriz
import androidx.compose.material.icons.filled.Tag
import androidx.compose.material.icons.filled.Usb
import androidx.compose.material3.Button
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.FilledTonalButton
import androidx.compose.material3.FilledTonalIconButton
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.ListItem
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.ModalBottomSheet
import androidx.compose.material3.OutlinedCard
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
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
import com.aquawius.aqua.AquaCounter
import com.aquawius.aqua.AquaDiagnostics
import com.aquawius.aqua.AquaRates
import com.aquawius.aqua.AquaRouteMode
import com.aquawius.aqua.AquaRuntimeState
import com.aquawius.aqua.AudioDeviceMonitor
import com.aquawius.aqua.formatHostPort
import com.aquawius.aqua.ui.theme.AquaTheme
import java.util.Locale
import kotlin.math.roundToInt

/** 首页：地址 + gRPC 端口 + 面向用户的核心指标（音频契约 / 连接质量 / 缓冲水位）。
 *  完整开发诊断在"高级"页；此处只保留用户关心的少数指标。 */
@Composable
fun AquaScreen(controller: AquaController, modifier: Modifier = Modifier) {
    var showDevicePicker by remember { mutableStateOf(false) }
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
                controller.rates,
                controller.connectResult,
                controller.sessionDurationMs,
                controller.effectiveHeartbeatIntervalMs,
            )
        }

        // 底部固定区：切换降级横幅 → 状态横幅 → 连接按钮 + 设备按钮。
        // 退出动画触发时 switchNotice 已置 null：缓存最后一份文案渲染，
        // 否则内容先空、退出动画作用在空盒子上（消失动画"不完整"）。
        var lastSwitchNotice by remember { mutableStateOf("") }
        controller.switchNotice?.let { lastSwitchNotice = it }
        AnimatedVisibility(
            visible = controller.switchNotice != null,
            enter = slideInVertically(tween(220)) { it / 2 } + fadeIn(tween(220)),
            exit = slideOutVertically(tween(200)) { it / 2 } + fadeOut(tween(160)),
        ) {
            SwitchNoticeBanner(lastSwitchNotice)
        }
        StatusBanner(controller)
        Row(
            horizontalArrangement = Arrangement.spacedBy(12.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            ConnectButton(controller, Modifier.weight(1f))
            // 播放设备入口（弹层）：贴近主操作，播放中 = 立即切换；
            // 未连接 = 选定起步目标设备（首流初始化前生效）。
            FilledTonalIconButton(
                onClick = { showDevicePicker = true },
                modifier = Modifier.size(52.dp),
            ) {
                Icon(Icons.Filled.Headphones, contentDescription = "播放设备")
            }
        }
    }

    if (showDevicePicker) {
        PlaybackDevicePicker(controller, onDismiss = { showDevicePicker = false })
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
private fun ConnectButton(controller: AquaController, modifier: Modifier = Modifier) {
    if (controller.isRunning) {
        FilledTonalButton(
            onClick = { controller.disconnect() },
            enabled = !controller.stopping,
            modifier = modifier.height(52.dp),
        ) {
            Icon(Icons.Filled.Stop, contentDescription = null)
            Spacer(Modifier.width(8.dp))
            Text("断开连接")
        }
    } else {
        Button(
            onClick = { controller.connect() },
            enabled = !controller.connecting && !controller.stopping,
            modifier = modifier.height(52.dp),
        ) {
            Icon(Icons.Filled.PlayArrow, contentDescription = null)
            Spacer(Modifier.width(8.dp))
            Text("连接")
        }
    }
}

/** 播放设备切换降级横幅（playback_switching_design.md §9）：目标失败但
 *  已兜底成功时的用户提示；由 Controller 自动过期清除。 */
@Composable
private fun SwitchNoticeBanner(text: String) {
    Surface(
        color = MaterialTheme.colorScheme.tertiaryContainer,
        contentColor = MaterialTheme.colorScheme.onTertiaryContainer,
        shape = MaterialTheme.shapes.large,
    ) {
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .padding(horizontal = 16.dp, vertical = 12.dp),
            horizontalArrangement = Arrangement.spacedBy(12.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Icon(Icons.Filled.SwapHoriz, contentDescription = null)
            Text(text, style = MaterialTheme.typography.bodyMedium)
        }
    }
}

/** 播放设备选择弹层：跟随系统 + 各输出设备（playback_switching_design.md §9）。
 *  当前选择 = 路由模式 + 请求设备；实际输出（stream 回读）显示在副标题。 */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun PlaybackDevicePicker(controller: AquaController, onDismiss: () -> Unit) {
    val routeMode = controller.diagnostics?.routeMode
    val requestedId = controller.requestedPlaybackDeviceId
    val streamName = controller.streamPlaybackDeviceId.takeIf { it.isNotEmpty() }
        ?.let { deviceDisplayName(controller, it) }
    // 未连接 = 选择起步目标设备（首流初始化前生效）；连接中/播放中 = 立即切换。
    val pickingInitial = !controller.isRunning
    val pendingId = controller.pendingPlaybackDeviceId

    ModalBottomSheet(onDismissRequest = onDismiss) {
        Column(
            Modifier
                .fillMaxWidth()
                .verticalScroll(rememberScrollState())
                .padding(bottom = 24.dp),
        ) {
            // 头部（当前输出）与列表项左缘对齐：ListItem 前导内容起点 16dp、
            // 图标 24dp + 间距 16dp → 标题与列表项 headline 同一条竖线（56dp）。
            Row(
                modifier = Modifier.padding(horizontal = 16.dp, vertical = 12.dp),
                horizontalArrangement = Arrangement.spacedBy(16.dp),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Icon(
                    Icons.Filled.Headphones,
                    contentDescription = null,
                    tint = MaterialTheme.colorScheme.primary,
                )
                Column {
                    Text(
                        "播放设备",
                        style = MaterialTheme.typography.titleMedium,
                        fontWeight = FontWeight.SemiBold,
                    )
                    Text(
                        if (pickingInitial) {
                            if (pendingId == AquaController.FOLLOW_SYSTEM_DEVICE_ID) "起步：跟随系统"
                            else "起步：${deviceDisplayName(controller, "android:$pendingId")}"
                        } else if (streamName != null) {
                            "当前输出：$streamName"
                        } else {
                            routeMode?.label ?: "跟随系统"
                        },
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                }
            }
            // 头部与选择列表之间唯一的分割线（选项之间不再加）。
            HorizontalDivider()
            ListItem(
                headlineContent = { Text("跟随系统") },
                supportingContent = {
                    Text(if (pickingInitial) "连接时按系统默认输出起步" else "系统默认输出变化时自动跟随")
                },
                leadingContent = {
                    Icon(Icons.Filled.Autorenew, contentDescription = null)
                },
                trailingContent = {
                    val selected = if (pickingInitial) {
                        pendingId == AquaController.FOLLOW_SYSTEM_DEVICE_ID
                    } else {
                        routeMode == null || routeMode == AquaRouteMode.FOLLOW_SYSTEM
                    }
                    if (selected) {
                        Icon(
                            Icons.Filled.Check,
                            contentDescription = "已选择",
                            tint = MaterialTheme.colorScheme.primary,
                        )
                    }
                },
                modifier = Modifier.clickable {
                    if (pickingInitial) {
                        controller.pendingPlaybackDeviceId = AquaController.FOLLOW_SYSTEM_DEVICE_ID
                    } else {
                        controller.setPlaybackDevice(AquaController.FOLLOW_SYSTEM_DEVICE_ID)
                    }
                    onDismiss()
                },
            )
            controller.playbackDevices.forEach { device ->
                val selected = if (pickingInitial) {
                    pendingId == device.id
                } else {
                    routeMode == AquaRouteMode.PREFERRED_DEVICE &&
                            requestedId == "android:${device.id}"
                }
                ListItem(
                    headlineContent = {
                        Text(device.productName?.toString()?.takeIf { it.isNotBlank() }
                            ?: AudioDeviceMonitor.typeLabel(device.type))
                    },
                    supportingContent = { Text(AudioDeviceMonitor.typeLabel(device.type)) },
                    leadingContent = {
                        Icon(deviceTypeIcon(device.type), contentDescription = null)
                    },
                    trailingContent = {
                        if (selected) {
                            Icon(
                                Icons.Filled.Check,
                                contentDescription = "已选择",
                                tint = MaterialTheme.colorScheme.primary,
                            )
                        }
                    },
                    modifier = Modifier.clickable {
                        if (pickingInitial) {
                            controller.pendingPlaybackDeviceId = device.id
                        } else {
                            controller.setPlaybackDevice(device.id)
                        }
                        onDismiss()
                    },
                )
            }
        }
    }
}

/** "android:N" -> 设备显示名（列表内匹配；未知回退 id 原文）。 */
private fun deviceDisplayName(controller: AquaController, idString: String): String {
    val id = idString.removePrefix("android:").toIntOrNull()
    val device = id?.let { needle -> controller.playbackDevices.firstOrNull { it.id == needle } }
    return device?.productName?.toString()?.takeIf { it.isNotBlank() }
        ?: AudioDeviceMonitor.typeLabel(device?.type ?: -1)
}

/** 设备类型图标（Kotlin 层分类，不进 C++）。 */
private fun deviceTypeIcon(type: Int): ImageVector = when (type) {
    android.media.AudioDeviceInfo.TYPE_BUILTIN_SPEAKER -> Icons.Filled.Speaker
    android.media.AudioDeviceInfo.TYPE_WIRED_HEADSET,
    android.media.AudioDeviceInfo.TYPE_WIRED_HEADPHONES -> Icons.Filled.Headset

    android.media.AudioDeviceInfo.TYPE_BLUETOOTH_A2DP,
    android.media.AudioDeviceInfo.TYPE_BLE_HEADSET -> Icons.Filled.BluetoothAudio

    android.media.AudioDeviceInfo.TYPE_USB_HEADSET,
    android.media.AudioDeviceInfo.TYPE_USB_DEVICE,
    android.media.AudioDeviceInfo.TYPE_USB_ACCESSORY -> Icons.Filled.Usb

    else -> Icons.Filled.GraphicEq
}


/** 核心指标：每张卡只放"自己模块"的 diagnostics，顺序 = 数据流
 *  （音频契约 → 会话 → 传输 → 抖动 → 缓冲 → 目标 → 输出 → 听感）。
 *  音频契约卡恒显（未连接时占位 "—"）；其余卡连接后出现。 */
@Composable
private fun MetricsSection(
    state: AquaRuntimeState,
    connecting: Boolean,
    d: AquaDiagnostics?,
    rates: AquaRates?,
    format: AquaConnectResult?,
    sessionDurationMs: Long?,
    /** 实际生效的心跳周期（ms）：「距上次 ACK」是相位量，需要分母才不被误读。 */
    heartbeatIntervalMs: Int,
) {
    // 音频契约卡固定占位，未连接时全部显示 "—"（同老版）。
    MetricGroupCard(
        title = "音频格式",
        icon = Icons.Filled.GraphicEq,
        metrics = audioMetrics(format),
    )

    if (d != null && format != null) {
        MetricGroupCard(
            "会话",
            Icons.Filled.Code,
            sessionMetrics(d, rates, format, sessionDurationMs, heartbeatIntervalMs)
        )
        MetricGroupCard("传输", Icons.Filled.SettingsEthernet, transportMetrics(d, rates))
        MetricGroupCard("网络抖动", Icons.Filled.NetworkCheck, jitterMetrics(d, rates))
        MetricGroupCard(
            "音频缓冲",
            Icons.Filled.Storage,
            bufferMetrics(d, rates),
            d.jbWaterLevel.toFloat()
        )
        MetricGroupCard("自适应缓冲", Icons.Filled.AutoGraph, adaptiveMetrics(d))
        MetricGroupCard("播放输出", Icons.Filled.SpeakerGroup, outputMetrics(d, rates))
        MetricGroupCard("音频后端", Icons.Filled.Memory, audioQualityMetrics(d, rates))
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

private data class MetricEntry(
    val label: String,
    val value: String,
    val fullRow: Boolean = false,
    /** 每秒速率（次/s 或 B/s）；null = 该指标无速率（瞬时量）或本拍算不出。 */
    val rate: String? = null,
)

/** 音频（协商契约）：采样率/声道/编码/位深/码率/帧长。
 *  只放"流建立时谈定的格式"；本地设备实际跑成什么样在「播放输出」卡，
 *  两者刻意分卡：一个是网络侧契约，一个是本机后端回读。 */
private fun audioMetrics(f: AquaConnectResult?): List<MetricEntry> {
    if (f == null) {
        return listOf(
            MetricEntry("采样率", "—"),
            MetricEntry("声道", "—"),
            MetricEntry("编码", "—"),
            MetricEntry("位深", "—"),
            MetricEntry("码率", "—"),
            MetricEntry("帧长 F", "—"),
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
        MetricEntry("采样率", sampleRateText),
        MetricEntry("声道", channelsText),
        MetricEntry("编码", f.encoding.label),
        MetricEntry(
            "位深",
            if (f.encoding.bitsPerSample > 0) "${f.encoding.bitsPerSample} bit" else "—"
        ),
        MetricEntry("码率", if (f.bitRateKbps > 0) "${f.bitRateKbps} kbps" else "—"),
        MetricEntry(
            "帧长 F", if (f.frameCount > 0) {
                String.format(Locale.US, "%.1f ms", f.frameCount * 1000.0 / f.sampleRate)
            } else {
                "—"
            }
        ),
    )
}

/** 连接（会话上下文）：链路 → 会话 ID → 时长 / ACK → 数据源单行。
 *  全部来自 heartbeat（net）与会话建立结果，不掺任何缓冲 / 音频侧的量。
 *  fullRow 标记"数据源"整行显示（IPv6 地址很长，两列布局会被截断）。 */
private fun sessionMetrics(
    d: AquaDiagnostics,
    r: AquaRates?,
    f: AquaConnectResult,
    durationMs: Long?,
    heartbeatIntervalMs: Int,
): List<MetricEntry> = listOf(
    MetricEntry(
        "链路",
        if (d.heartbeatFailed) "中断" else if (d.heartbeatAckMisses > 0) "波动" else "正常"
    ),
    MetricEntry("会话 ID", String.format(Locale.US, "%08x", f.sessionId)),
    MetricEntry("时长", durationMs?.let { formatDuration(it) } ?: "—"),
    MetricEntry("ACK 总数", d.heartbeatAckCount.f0(), rate = r.count(AquaCounter.HeartbeatAcks)),
    // 负值 = 尚未收到任何 ACK（握手期）。这是一个**相位**量而不是周期量：心跳每
    // interval 发一次，1s 的采样落在周期内的固定相位上，所以稳定显示 300ms 是
    // 正常的（不是心跳周期变短）。分母一起显示，避免被误读成"心跳只有 300ms"。
    MetricEntry(
        "距上次 ACK",
        if (d.heartbeatAckAgeMs < 0) {
            "—"
        } else {
            "${d.heartbeatAckAgeMs.f0()} / $heartbeatIntervalMs ms"
        },
    ),
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

/** 传输（UDP 数据面）：收 → 发 → 流量 → 接收侧断档 → 发送侧故障。
 *  "流缺口 / 缺失帧"是接收序列断档（UdpClient 口径："收到流少了号"，
 *  不等于丢包），与 JB 的欠载是两个层次：这里看网络，那边看播放。 */
private fun transportMetrics(d: AquaDiagnostics, r: AquaRates?): List<MetricEntry> = listOf(
    MetricEntry("音频包数", d.audioFramesAccepted.f0(), rate = r.count(AquaCounter.AudioPackets)),
    MetricEntry("收包总数", d.rxPackets.f0(), rate = r.count(AquaCounter.RxPackets)),
    MetricEntry("发包总数", d.txPackets.f0(), rate = r.count(AquaCounter.TxPackets)),
    MetricEntry("上行流量", d.txBytes.fBytes(), rate = r.bytes(AquaCounter.TxBytes)),
    MetricEntry("下行流量", d.rxBytes.fBytes(), rate = r.bytes(AquaCounter.RxBytes)),
    MetricEntry("流缺口", d.rxSequenceGapEvents.f0(), rate = r.count(AquaCounter.RxGapEvents)),
    MetricEntry(
        "缺失帧",
        d.rxSequenceMissingFrames.f0(),
        rate = r.count(AquaCounter.RxMissingFrames)
    ),
    MetricEntry("发送丢弃", d.txDropped.f0(), rate = r.count(AquaCounter.TxDropped)),
    MetricEntry(
        "发送失败",
        d.txEnqueueFailures.f0(),
        rate = r.count(AquaCounter.TxEnqueueFailures)
    ),
)

/** 网络抖动（JitterEstimator，观测层）：路径特征 ＋ 断流尾部。
 *  判缺口性质看两者谁大：J 大 = 抖动超出预测（该调 k）；
 *  断流峰值大而 J 小 = 尾部被 stall 门剔除（该调峰值上限）。 */
private fun jitterMetrics(d: AquaDiagnostics, r: AquaRates?): List<MetricEntry> = listOf(
    MetricEntry("抖动均值 J", String.format(Locale.US, "%.2f ms", d.estimatorJitterMs)),
    MetricEntry("路径底噪", String.format(Locale.US, "%.1f ms", d.estimatorBaseDelayMs)),
    MetricEntry("相对时延", String.format(Locale.US, "%.1f ms", d.estimatorTransitMs)),
    MetricEntry("包间隔", String.format(Locale.US, "%.2f ms", d.jcArrivalIntervalMs)),
    MetricEntry("断流峰值", String.format(Locale.US, "%.1f ms", d.jcStallPeakMs)),
    MetricEntry("最近断流", String.format(Locale.US, "%.1f ms", d.jcLastStallGapMs)),
    MetricEntry("断流剔除", d.jcStallEvents.f0(), rate = r.count(AquaCounter.StallEvents)),
    MetricEntry(
        "乱序·重复·迟到",
        "${d.estimatorReorderedPackets.f0()} · ${d.estimatorDuplicatePackets.f0()} · ${d.estimatorLatePackets.f0()}",
        rate = r.counts(AquaCounter.Reordered, AquaCounter.Duplicate, AquaCounter.Late),
    ),
)

/** 缓冲（JitterBuffer 机械）：水位与阈值 → 当前动作 → 修正计数。
 *  这张卡只讲"队列自身"（多满、在哪个带、做了什么修正）；
 *  "吐出来的是什么"（静音 / 掩盖）在「播放输出」与「音质」两张卡。
 *  Fill = 低水位：重复/停住以减慢播放；Drop = 高水位：跳过整槽追上。 */
private fun bufferMetrics(d: AquaDiagnostics, r: AquaRates?): List<MetricEntry> = listOf(
    MetricEntry("占用槽位", "${d.jbUsedSlots}/${d.jbCapacitySlots}"),
    MetricEntry("缓存水位", String.format(Locale.US, "%.0f%%", d.jbWaterLevel * 100)),
    // 带随 target 等比缩放：lead 落在正常带内不动手，所以看动作之前先看带。
    MetricEntry("正常带", "${d.jcBandNormalLow} ~ ${d.jcBandNormalHigh} 槽"),
    MetricEntry("告警带", "${d.jcBandWarningLow} / ${d.jcBandWarningHigh} 槽"),
    MetricEntry("当前动作", d.episodeStateLabel),
    MetricEntry("Fill 次数", d.jbFillEpisodes.f0(), rate = r.count(AquaCounter.FillEpisodes)),
    MetricEntry("Drop 次数", d.jbDropEpisodes.f0(), rate = r.count(AquaCounter.DropEpisodes)),
    MetricEntry("重锚定", d.jbReanchorCount.f0(), rate = r.count(AquaCounter.Reanchor)),
    MetricEntry(
        "迟到拒收",
        d.jbPushRejectedLate.f0(),
        rate = r.count(AquaCounter.PushRejectedLate)
    ),
    MetricEntry("拒收总数", d.jbPushRejected.f0(), rate = r.count(AquaCounter.PushRejected)),
    MetricEntry("Fill 槽数", d.jbFillCorrectedSlots.f0(), rate = r.count(AquaCounter.FillSlots)),
    MetricEntry("Drop 槽数", d.jbDropSkippedSlots.f0(), rate = r.count(AquaCounter.DropSlots)),
    // crossfade 拼接次数：判断"是否真的在拼接、有没有连发"，此前只能靠耳朵。
    MetricEntry("拼接次数", d.jbSpliceEvents.f0(), rate = r.count(AquaCounter.SpliceEvents)),
)

/** 自适应缓冲（TargetController）：现值 → 期望值 → 为什么。
 *  固定模式（--jb-fixed-target）下 controller 根本不创建，只显示现状；
 *  "期望 target ≠ target"就是被限速 / 涨后锁跌 / 死区按住，具体看"状态"。 */
private fun adaptiveMetrics(d: AquaDiagnostics): List<MetricEntry> {
    if (!d.jcAdaptive) {
        return listOf(
            MetricEntry("模式", "固定"),
            MetricEntry("实际 target", "${d.targetSlots} 槽"),
            MetricEntry("目标延迟", String.format(Locale.US, "%.0f ms", d.targetMs)),
            MetricEntry("实际 lead", "${d.jbLeadSlots} 槽"),
        )
    }
    val entries = mutableListOf(
        MetricEntry("实际 target", "${d.targetSlots} 槽"),
        MetricEntry("目标延迟", String.format(Locale.US, "%.0f ms", d.targetMs)),
        MetricEntry("实际 lead", "${d.jbLeadSlots} 槽"),
        MetricEntry("期望 target", "${d.jcDesiredSlots} 槽"),
        MetricEntry("可用区间", "${d.jcMinSlots} ~ ${d.jcMaxSlots} 槽"),
        MetricEntry("余量来源", d.marginSourceLabel),
        MetricEntry("状态", d.targetStateLabel),
        // 常驻一行（干净链路上恒为 +0.0）：之前只在 >0 时才占位，卡片行数会
        // 在 7/8 之间来回变，界面跳动，而且"闭环到底有没有在工作"看不到。
        MetricEntry("欠载补偿", String.format(Locale.US, "+%.1f 槽", d.jcUnderrunPenalty)),
    )
    return entries
}

/** 播放输出（本地播放链路）：设备流参数 ＋ 拉出来的音频。
 *  "静音帧"是 JB 为修正时间轴（预滚 / 低水位 Hold）主动吐的静音，
 *  与「音质」卡的欠载 / 掩盖（没数据可用）是两回事，故归这里不给音质卡。
 *  设备 XRun 是 AAudio 底层的欠载/超限计数（与 JB 欠载无关的独立信号）。 */
private fun outputMetrics(d: AquaDiagnostics, r: AquaRates?): List<MetricEntry> {
    val output = listOf(
        MetricEntry("输出帧", d.jbPullFrames.f0(), rate = r.count(AquaCounter.PullFrames)),
        MetricEntry(
            "静音帧",
            d.jbPullSilenceFrames.f0(),
            rate = r.count(AquaCounter.PullSilenceFrames)
        ),
        MetricEntry("静音占比", String.format(Locale.US, "%.3f%%", d.silenceRatio * 100)),
        MetricEntry("当前静音连续", "${d.jbConsecutiveSilenceFrames} 帧"),
        MetricEntry("最长静音连续", "${d.jbMaxSilenceRunFrames} 帧"),
    )
    if (d.streamBackend == 0) {
        return listOf(
            MetricEntry("性能模式", "—"),
            MetricEntry("Burst", "—"),
            MetricEntry("设备缓冲容量", "—"),
            MetricEntry("设备 XRun", "—"),
        ) + output
    }
    val performance = when (d.streamPerformanceMode) {
        12 -> "低延迟"
        11 -> "省电"
        10 -> if (d.streamBackend == 2) "标准" else "无"
        else -> "—"
    }
    val burst = if (d.streamFramesPerBurst > 0) "${d.streamFramesPerBurst} 帧" else "—"
    val capacity =
        if (d.streamBufferCapacityFrames > 0) "${d.streamBufferCapacityFrames} 帧" else "—"
    return listOf(
        MetricEntry("性能模式", performance),
        MetricEntry("Burst", burst),
        MetricEntry("设备缓冲容量", capacity),
        MetricEntry("设备 XRun", d.streamXrunCount.f0(), rate = r.count(AquaCounter.StreamXrun)),
    ) + output
}

/** 音质（听感损伤）：只放"没数据可用"造成的失真——欠载、掩盖、缺帧。
 *  时间轴修正的静音在「播放输出」，不在这里重复计数。 */
private fun audioQualityMetrics(d: AquaDiagnostics, r: AquaRates?): List<MetricEntry> {
    val current = when {
        d.concealing -> "掩盖中（${d.jcConcealRunSlots} 槽）"
        d.underrunning -> "缺帧中（${d.jcUnderrunRunSlots} 槽）"
        else -> "正常"
    }
    return listOf(
        MetricEntry("当前状态", current),
        MetricEntry("欠载率", String.format(Locale.US, "%.3f%%", d.jbUnderrunRatio * 100)),
        MetricEntry(
            "欠载次数",
            d.jbUnderrunEvents.f0(),
            rate = r.count(AquaCounter.UnderrunEvents)
        ),
        MetricEntry("最长连续缺帧", "${d.jbMaxConsecutiveUnderrunSlots} 槽"),
        MetricEntry(
            "掩盖槽数",
            d.jbConcealedSlots.f0(),
            rate = r.count(AquaCounter.ConcealedSlots)
        ),
        MetricEntry(
            "掩盖转静音",
            d.jbConcealedSaturatedSlots.f0(),
            rate = r.count(AquaCounter.ConcealedSaturated)
        ),
    )
}

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
                Spacer(Modifier.weight(1f))
                if (progress != null) {
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
        // 速率（纯 Kotlin 差分派生）：累计值看不出"此刻是否在恶化"，速率才看得出来。
        if (entry.rate != null) {
            Text(
                entry.rate,
                style = MaterialTheme.typography.labelMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
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

// ---- 速率（AquaRates 派生量；差分实现见 AquaRates.kt）----

/** 计数速率文本：<0.05 → "0/s"（干净链路也别空着，好和"本拍无值"区分），<10 一位小数，否则取整。 */
private fun Double.fRate(): String = when {
    this < 0.05 -> "0/s"
    this < 10.0 -> String.format(Locale.US, "%.1f/s", this)
    else -> String.format(Locale.US, "%.0f/s", this)
}

/** 字节速率文本：B/s → GB/s 自动升档。 */
private fun Double.fBytesRate(): String = when {
    this >= 1e9 -> String.format(Locale.US, "%.2f GB/s", this / 1e9)
    this >= 1e6 -> String.format(Locale.US, "%.2f MB/s", this / 1e6)
    this >= 1e3 -> String.format(Locale.US, "%.1f KB/s", this / 1e3)
    else -> String.format(Locale.US, "%.0f B/s", this)
}

/** 次/s 速率；表缺失或本拍无值 → null（UI 不渲染速率行）。 */
private fun AquaRates?.count(counter: AquaCounter): String? = this?.get(counter)?.fRate()

/** 多计数器合并速率（顺序与值一致；全部无值 → null，部分无值用 "—" 占位）。 */
private fun AquaRates?.counts(vararg counters: AquaCounter): String? {
    if (this == null) return null
    val parts = counters.map { c -> this[c]?.fRate() }
    if (parts.all { it == null }) return null
    return parts.joinToString(" · ") { it ?: "—" }
}

/** 字节/s 速率。 */
private fun AquaRates?.bytes(counter: AquaCounter): String? = this?.get(counter)?.fBytesRate()

@Preview(showBackground = true)
@Composable
private fun AquaScreenPreview() {
    AquaTheme {
        AquaScreen(remember { AquaController() })
    }
}
