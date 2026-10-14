package com.aquawius.aqua.ui

import android.content.Context
import android.content.Intent
import android.os.Build
import android.os.PowerManager
import android.provider.Settings
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.OpenInNew
import androidx.compose.material.icons.filled.ArrowDropDown
import androidx.compose.material.icons.filled.Check
import androidx.compose.material.icons.filled.ChevronRight
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.ExposedDropdownMenuAnchorType
import androidx.compose.material3.ExposedDropdownMenuBox
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.ListItem
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedCard
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalInspectionMode
import androidx.compose.ui.platform.LocalUriHandler
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.tooling.preview.Preview
import androidx.compose.ui.unit.dp
import androidx.core.app.NotificationManagerCompat
import androidx.core.net.toUri
import androidx.lifecycle.compose.LifecycleResumeEffect
import com.aquawius.aqua.AquaController
import com.aquawius.aqua.ui.theme.AquaTheme
import com.aquawius.aqua.ui.theme.AquaThemeMode
import com.aquawius.aqua.ui.theme.AquaThemeStyle

/** 设置：连接 / 电池（实时状态）/ 外观（配色 + 主题）/ GitHub + 关于入口。 */
@Composable
fun SettingsScreen(
    controller: AquaController,
    themeStyle: AquaThemeStyle,
    onThemeStyleChange: (AquaThemeStyle) -> Unit,
    themeMode: AquaThemeMode,
    onThemeModeChange: (AquaThemeMode) -> Unit,
    onAboutClick: () -> Unit,
    modifier: Modifier = Modifier,
) {
    val context = LocalContext.current
    val uriHandler = LocalUriHandler.current

    Column(
        modifier = modifier
            .fillMaxSize()
            .verticalScroll(rememberScrollState())
            .padding(16.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        SectionHeader("连接")
        OutlinedCard(Modifier.fillMaxWidth()) {
            Column {
                SettingSwitch(
                    title = "自动重连",
                    subtitle = "断线后尝试重连",
                    checked = controller.autoReconnect,
                    onCheckedChange = { controller.autoReconnect = it },
                )
            }
        }

        SectionHeader("播放")
        OutlinedCard(Modifier.fillMaxWidth()) {
            Column {
                SettingSwitch(
                    title = "播放时屏幕常亮",
                    subtitle = "播放期间保持屏幕常亮",
                    checked = controller.keepScreenOn,
                    onCheckedChange = { controller.keepScreenOn = it },
                )
                InsetDivider()
                SettingSwitch(
                    title = "允许同时播放",
                    subtitle = "允许其他音乐 App 同时播放",
                    checked = controller.allowSimultaneousPlayback,
                    onCheckedChange = { controller.allowSimultaneousPlayback = it },
                )
                InsetDivider()
                SettingSwitch(
                    title = "低延迟模式",
                    subtitle = "低延迟模式可能会和手机音效冲突，默认开启，下次连接生效。\n" +
                            "注意，关闭本模式后，音频核心需要的数据量将大幅增加，默认数值可能不能满足设备需求，" +
                            "会持续变卡、音质变差，请在高级页把「最低水位」调到40槽以上，「缓冲容量」调到80槽以上.",
                    checked = controller.playbackLowLatency,
                    onCheckedChange = { controller.playbackLowLatency = it },
                )
            }
        }


        SectionHeader("通知")
        NotificationCard()

        SectionHeader("电池")
        BatteryCard()

        SectionHeader("外观")
        OutlinedCard(Modifier.fillMaxWidth()) {
            Column {
                DropdownRow(
                    title = "配色",
                    subtitle = "应用颜色方案",
                    currentLabel = themeStyle.displayLabel(),
                    options = paletteOptions(),
                    isSelected = { themeStyle == it },
                    onSelect = { onThemeStyleChange(it as AquaThemeStyle) },
                )
                InsetDivider()
                DropdownRow(
                    title = "主题",
                    subtitle = "界面设计语言",
                    currentLabel = themeMode.label,
                    options = themeModeOptions(),
                    isSelected = { themeMode == it },
                    onSelect = { onThemeModeChange(it as AquaThemeMode) },
                )
            }
        }

        SectionHeader("日志")
        OutlinedCard(Modifier.fillMaxWidth()) {
            Column {
                DropdownRow(
                    title = "日志级别",
                    subtitle = "系统日志输出级别，下次连接生效",
                    currentLabel = logLevelLabel(controller.logLevel),
                    options = logLevelOptions(),
                    isSelected = { it == controller.logLevel },
                    onSelect = { controller.logLevel = it as Int },
                )
            }
        }

        SectionHeader("关于")
        OutlinedCard(Modifier.fillMaxWidth()) {
            Column {
                ListItem(
                    headlineContent = { Text("GitHub") },
                    supportingContent = { Text("github.com/aquawius/aqua") },
                    trailingContent = {
                        Icon(Icons.AutoMirrored.Filled.OpenInNew, contentDescription = null)
                    },
                    modifier = Modifier.clickable {
                        runCatching { uriHandler.openUri("https://github.com/aquawius/aqua") }
                    },
                )
                InsetDivider()
                ListItem(
                    headlineContent = { Text("关于 Aqua") },
                    supportingContent = { Text("版本与设备信息") },
                    trailingContent = {
                        Icon(Icons.Filled.ChevronRight, contentDescription = null)
                    },
                    modifier = Modifier.clickable(onClick = onAboutClick),
                )
            }
        }
    }
}

/** 通知设置：显示应用通知开关真实状态，点击跳系统通知设置页。 */
@Composable
private fun NotificationCard() {
    val context = LocalContext.current
    // 预览环境（layoutlib）不支持 NotificationManager 查询，直接给静态值。
    val inPreview = LocalInspectionMode.current
    var enabled by remember { mutableStateOf(!inPreview && areNotificationsEnabled(context)) }

    // 从系统设置页返回时刷新状态。
    if (!inPreview) {
        LifecycleResumeEffect(Unit) {
            enabled = areNotificationsEnabled(context)
            onPauseOrDispose { }
        }
    }

    OutlinedCard(Modifier.fillMaxWidth()) {
        ListItem(
            headlineContent = { Text("通知设置") },
            supportingContent = {
                Text(
                    "当前状态：${if (enabled) "已启用" else "已禁用（通知栏播放控制不可用）"}",
                )
            },
            trailingContent = {
                Icon(Icons.Filled.ChevronRight, contentDescription = null)
            },
            modifier = Modifier.clickable {
                // 跳到本应用的系统通知设置页（用户可自行开关联通知渠道）。
                runCatching {
                    context.startActivity(
                        Intent(Settings.ACTION_APP_NOTIFICATION_SETTINGS)
                            .putExtra(Settings.EXTRA_APP_PACKAGE, context.packageName)
                    )
                }
            },
        )
    }
}

private fun areNotificationsEnabled(context: Context): Boolean =
    NotificationManagerCompat.from(context).areNotificationsEnabled()

/** 忽略电池优化：导航行（授予走系统授权对话框、撤销走系统设置），
 *  状态反映系统真实授权情况，返回本页时自动刷新。 */
@Composable
private fun BatteryCard() {
    val context = LocalContext.current
    // 预览环境（layoutlib）不支持 PowerManager 系统服务，查询会崩溃，直接给静态值。
    val inPreview = LocalInspectionMode.current
    var ignoring by remember { mutableStateOf(!inPreview && isIgnoringBatteryOptimizations(context)) }

    // 从系统授权页/系统设置返回时刷新状态。
    if (!inPreview) {
        LifecycleResumeEffect(Unit) {
            ignoring = isIgnoringBatteryOptimizations(context)
            onPauseOrDispose { }
        }
    }

    OutlinedCard(Modifier.fillMaxWidth()) {
        // 刻意不做成 Switch：授予只能由系统授权对话框完成（点一下弹"允许/拒绝"），
        // 撤销则没有任何 API（只能跳系统设置列表让用户手动移除）。做成开关就是
        // "拨了一半不响应"的假开关——与通知卡统一为导航行：状态文本 + 点击走
        // 当前状态对应的系统入口。
        ListItem(
            headlineContent = { Text("忽略电池优化") },
            supportingContent = {
                Text(
                    if (ignoring) {
                        "已授权：后台长时间播放不会被系统限制。如需撤销，请前往系统设置手动移除。"
                    } else {
                        "未授权：后台长时间播放可能被系统限制。点击授权。"
                    }
                )
            },
            trailingContent = {
                Icon(Icons.Filled.ChevronRight, contentDescription = null)
            },
            modifier = Modifier.clickable {
                runCatching {
                    if (ignoring) {
                        context.startActivity(
                            Intent(Settings.ACTION_IGNORE_BATTERY_OPTIMIZATION_SETTINGS)
                        )
                    } else {
                        // 直接弹系统授权对话框（一步授予）；个别 OEM 不响应该 intent 时
                        // 回退到电池优化列表，让用户手动勾选。
                        runCatching {
                            context.startActivity(
                                Intent(
                                    Settings.ACTION_REQUEST_IGNORE_BATTERY_OPTIMIZATIONS,
                                    "package:${context.packageName}".toUri(),
                                )
                            )
                        }.onFailure {
                            context.startActivity(
                                Intent(Settings.ACTION_IGNORE_BATTERY_OPTIMIZATION_SETTINGS)
                            )
                        }
                    }
                }
            },
        )
    }
}

private fun isIgnoringBatteryOptimizations(context: Context): Boolean =
    (context.getSystemService(Context.POWER_SERVICE) as? PowerManager)
        ?.isIgnoringBatteryOptimizations(context.packageName) ?: false

@Composable
private fun InsetDivider() {
    HorizontalDivider(modifier = Modifier.padding(start = 16.dp))
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

/** 下拉可选项：文案 + 是否可用（Miuix 等预留项禁用）。 */
private data class DropdownOption(val label: String, val value: Any, val enabled: Boolean = true)

private fun paletteOptions(): List<DropdownOption> = buildList {
    add(DropdownOption("Aqua 青绿", AquaThemeStyle.AQUA))
    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
        add(DropdownOption("动态取色", AquaThemeStyle.DYNAMIC))
    }
    add(DropdownOption("经典紫", AquaThemeStyle.CLASSIC))
}

private fun themeModeOptions(): List<DropdownOption> = listOf(
    DropdownOption("Material", AquaThemeMode.MATERIAL),
    DropdownOption("Miuix（暂未支持）", AquaThemeMode.MIUIX, enabled = false), // 预留，暂未实现
)

private fun AquaThemeStyle.displayLabel(): String = paletteOptions()
    .firstOrNull { it.value == this }?.label ?: "Aqua 青绿"

/**
 * 系统日志级别可选项（C API AQUA_LOG_*）。
 *
 * 用法是**先在设置里选好级别、再连接**：级别是连接参数，`aqua_client_create` 时才生效；
 * 提前选好 Trace / Debug 再点连接，core（spdlog）的日志就是好的。
 *
 * 「默认」= `-1` = **不指定级别（什么都不做）**：C API 只在 `log_level >= 0` 时才调
 * `set_log_level`，所以本项就是"不覆盖进程当前级别"——首次连接时进程刚起来、即为
 * spdlog 的 Info 默认。这是**刻意的设计**，别改成"重置为 Info"或别的名字：
 * 进程级 logger 的级别一旦被 set 过就没有"重置"入口，也不需要——想换级别就再选一次、
 * 重连即可。
 */
private fun logLevelOptions(): List<DropdownOption> = listOf(
    DropdownOption("默认", -1),
    DropdownOption("Trace", 0),
    DropdownOption("Debug", 1),
    DropdownOption("Info", 2),
    DropdownOption("Warn", 3),
    DropdownOption("Error", 4),
    DropdownOption("Fatal", 5),
)

private fun logLevelLabel(code: Int): String =
    logLevelOptions().firstOrNull { it.value == code }?.label ?: "默认"

/** "关于"样式行 + 右侧当前值：M3 官方 ExposedDropdownMenuBox，锚点为
 *  右侧"当前值 + 箭头"区域（菜单宽度与位置随锚点，窄且贴其正下方）。 */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun DropdownRow(
    title: String,
    subtitle: String,
    currentLabel: String,
    options: List<DropdownOption>,
    isSelected: (Any) -> Boolean,
    onSelect: (Any) -> Unit,
) {
    var expanded by remember { mutableStateOf(false) }

    Row(
        modifier = Modifier
            .fillMaxWidth()
            .padding(horizontal = 16.dp, vertical = 12.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Column(Modifier.weight(1f).padding(end = 16.dp)) {
            Text(title, style = MaterialTheme.typography.bodyLarge)
            Text(
                subtitle,
                style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
        ExposedDropdownMenuBox(
            expanded = expanded,
            onExpandedChange = { expanded = it },
        ) {
            Row(
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(4.dp),
                modifier = Modifier.menuAnchor(ExposedDropdownMenuAnchorType.PrimaryNotEditable),
            ) {
                Text(
                    currentLabel,
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.primary,
                )
                Icon(
                    Icons.Filled.ArrowDropDown,
                    contentDescription = null,
                    tint = MaterialTheme.colorScheme.primary,
                )
            }
            DropdownMenu(
                expanded = expanded,
                onDismissRequest = { expanded = false },
            ) {
                options.forEach { option ->
                    DropdownMenuItem(
                        text = { Text(option.label) },
                        enabled = option.enabled,
                        onClick = {
                            expanded = false
                            if (option.enabled) onSelect(option.value)
                        },
                        trailingIcon = if (isSelected(option.value)) {
                            { Icon(Icons.Filled.Check, contentDescription = null) }
                        } else {
                            null
                        },
                    )
                }
            }
        }
    }
}

@Composable
private fun SettingSwitch(
    title: String,
    subtitle: String,
    checked: Boolean,
    onCheckedChange: (Boolean) -> Unit,
) {
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .padding(horizontal = 16.dp, vertical = 12.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Column(Modifier.weight(1f).padding(end = 16.dp)) {
            Text(title, style = MaterialTheme.typography.bodyLarge)
            Text(
                subtitle,
                style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
        Switch(checked = checked, onCheckedChange = onCheckedChange)
    }
}

@Preview(showBackground = true)
@Composable
private fun SettingsScreenPreview() {
    AquaTheme {
        SettingsScreen(
            controller = remember { AquaController() },
            themeStyle = AquaThemeStyle.AQUA,
            onThemeStyleChange = {},
            themeMode = AquaThemeMode.MATERIAL,
            onThemeModeChange = {},
            onAboutClick = {},
        )
    }
}
