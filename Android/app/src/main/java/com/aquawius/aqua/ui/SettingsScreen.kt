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
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.ChevronRight
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.ListItem
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedCard
import androidx.compose.material3.RadioButton
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.tooling.preview.Preview
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.LifecycleResumeEffect
import com.aquawius.aqua.AquaController
import com.aquawius.aqua.ui.theme.AquaTheme
import com.aquawius.aqua.ui.theme.AquaThemeMode
import com.aquawius.aqua.ui.theme.AquaThemeStyle

/** 设置：连接 / 电池（实时状态）/ 外观（配色 + 主题）/ 关于入口。 */
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
                    subtitle = "断线后指数退避重连",
                    checked = controller.autoReconnect,
                    onCheckedChange = { controller.autoReconnect = it },
                )
                InsetDivider()
                SettingSwitch(
                    title = "播放时屏幕常亮",
                    subtitle = "播放期间保持屏幕常亮",
                    checked = controller.keepScreenOn,
                    onCheckedChange = { controller.keepScreenOn = it },
                )
            }
        }

        SectionHeader("电池")
        BatteryCard()

        SectionHeader("外观")
        OutlinedCard(Modifier.fillMaxWidth()) {
            Column(Modifier.padding(vertical = 8.dp)) {
                SubHeader("配色")
                ThemeOptionRow(
                    title = "Aqua 青绿",
                    subtitle = "品牌默认配色",
                    selected = themeStyle == AquaThemeStyle.AQUA,
                    onClick = { onThemeStyleChange(AquaThemeStyle.AQUA) },
                )
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
                    ThemeOptionRow(
                        title = "动态取色",
                        subtitle = "跟随系统 Material You 壁纸配色",
                        selected = themeStyle == AquaThemeStyle.DYNAMIC,
                        onClick = { onThemeStyleChange(AquaThemeStyle.DYNAMIC) },
                    )
                }
                ThemeOptionRow(
                    title = "经典紫",
                    subtitle = "Material 3 基线配色",
                    selected = themeStyle == AquaThemeStyle.CLASSIC,
                    onClick = { onThemeStyleChange(AquaThemeStyle.CLASSIC) },
                )

                HorizontalDivider(Modifier.padding(vertical = 8.dp))
                SubHeader("主题")
                ThemeOptionRow(
                    title = "Material",
                    subtitle = "Material 3 设计语言",
                    selected = themeMode == AquaThemeMode.MATERIAL,
                    onClick = { onThemeModeChange(AquaThemeMode.MATERIAL) },
                )
                // Miuix：预留选项，暂未实现。
                ThemeOptionRow(
                    title = "Miuix",
                    subtitle = "敬请期待",
                    selected = themeMode == AquaThemeMode.MIUIX,
                    enabled = false,
                    onClick = { },
                )
            }
        }

        SectionHeader("关于")
        OutlinedCard(Modifier.fillMaxWidth()) {
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

/** 忽略电池优化：开关反映系统真实状态，返回本页时自动刷新。 */
@Composable
private fun BatteryCard() {
    val context = LocalContext.current
    var ignoring by remember { mutableStateOf(isIgnoringBatteryOptimizations(context)) }

    // 从系统授权页/系统设置返回时刷新状态。
    LifecycleResumeEffect(Unit) {
        ignoring = isIgnoringBatteryOptimizations(context)
        onPauseOrDispose { }
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

/** 卡片内子分组标题。 */
@Composable
private fun SubHeader(text: String) {
    Text(
        text,
        style = MaterialTheme.typography.labelLarge,
        fontWeight = FontWeight.Medium,
        color = MaterialTheme.colorScheme.onSurfaceVariant,
        modifier = Modifier.padding(start = 16.dp, top = 4.dp, bottom = 4.dp),
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

@Composable
private fun ThemeOptionRow(
    title: String,
    subtitle: String,
    selected: Boolean,
    enabled: Boolean = true,
    onClick: () -> Unit,
) {
    ListItem(
        headlineContent = { Text(title) },
        supportingContent = { Text(subtitle) },
        leadingContent = {
            RadioButton(selected = selected, onClick = onClick, enabled = enabled)
        },
        modifier = if (enabled) Modifier.clickable(onClick = onClick) else Modifier,
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
