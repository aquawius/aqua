package com.aquawius.aqua.ui

import android.content.Context
import android.content.Intent
import android.net.Uri
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
import androidx.core.app.NotificationManagerCompat
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
                InsetDivider()
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

/** 忽略电池优化：开关反映系统真实状态，返回本页时自动刷新。 */
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
        ListItem(
            headlineContent = { Text("忽略电池优化") },
            supportingContent = {
                Text(
                    "当前状态：${if (ignoring) "已忽略" else "未忽略（后台可能被限制）"}",
                )
            },
            trailingContent = {
                Switch(
                    checked = ignoring,
                    onCheckedChange = { checked ->
                        if (checked) {
                            // 请求加入电池优化白名单（系统弹窗确认）。
                            runCatching {
                                context.startActivity(
                                    Intent(
                                        Settings.ACTION_REQUEST_IGNORE_BATTERY_OPTIMIZATIONS,
                                        Uri.parse("package:${context.packageName}"),
                                    )
                                )
                            }
                        } else {
                            // 系统不提供反向 API，跳到电池优化列表让用户手动恢复。
                            runCatching {
                                context.startActivity(
                                    Intent(Settings.ACTION_IGNORE_BATTERY_OPTIMIZATION_SETTINGS)
                                )
                            }
                        }
                    },
                )
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
    DropdownOption("Miuix", AquaThemeMode.MIUIX, enabled = false), // 预留，暂未实现
)

private fun AquaThemeStyle.displayLabel(): String = paletteOptions()
    .firstOrNull { it.value == this }?.label ?: "Aqua 青绿"

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

    ListItem(
        headlineContent = { Text(title) },
        supportingContent = { Text(subtitle) },
        trailingContent = {
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
        },
    )
}

@Composable
private fun SettingSwitch(
    title: String,
    subtitle: String,
    checked: Boolean,
    onCheckedChange: (Boolean) -> Unit,
) {
    ListItem(
        headlineContent = { Text(title) },
        supportingContent = { Text(subtitle) },
        trailingContent = { Switch(checked = checked, onCheckedChange = onCheckedChange) },
    )
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
