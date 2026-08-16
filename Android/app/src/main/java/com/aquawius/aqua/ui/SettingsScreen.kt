package com.aquawius.aqua.ui

import android.content.Intent
import android.net.Uri
import android.os.Build
import android.provider.Settings
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.ListItem
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedCard
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.remember
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.tooling.preview.Preview
import androidx.compose.ui.unit.dp
import com.aquawius.aqua.AquaController
import com.aquawius.aqua.native.AquaNative
import com.aquawius.aqua.ui.theme.AquaTheme

/** 设置：M3 ListItem 分组卡片 + 关于。 */
@Composable
fun SettingsScreen(controller: AquaController, modifier: Modifier = Modifier) {
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
                InsetDivider()
                SettingSwitch(
                    title = "忽略电池优化",
                    subtitle = "请求系统忽略电池优化",
                    checked = controller.ignoreBatteryOptimization,
                    onCheckedChange = { checked ->
                        controller.ignoreBatteryOptimization = checked
                        if (checked) {
                            runCatching {
                                context.startActivity(
                                    Intent(
                                        Settings.ACTION_REQUEST_IGNORE_BATTERY_OPTIMIZATIONS,
                                        Uri.parse("package:${context.packageName}"),
                                    )
                                )
                            }
                        }
                    },
                )
            }
        }

        SectionHeader("关于")
        OutlinedCard(Modifier.fillMaxWidth()) {
            Column {
                AboutRow("设备", "${Build.MANUFACTURER} ${Build.MODEL}")
                InsetDivider()
                AboutRow("Android", "${Build.VERSION.RELEASE} (API ${Build.VERSION.SDK_INT})")
                InsetDivider()
                AboutRow("App 版本", appVersion(context))
                InsetDivider()
                AboutRow("Aqua 库版本", aquaVersion())
            }
        }
    }
}

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
private fun AboutRow(label: String, value: String) {
    ListItem(
        headlineContent = { Text(label) },
        trailingContent = {
            Text(
                value,
                style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        },
    )
}

@Composable
private fun appVersion(context: android.content.Context): String = remember {
    runCatching {
        context.packageManager.getPackageInfo(context.packageName, 0).versionName
    }.getOrNull() ?: "?"
}

@Composable
private fun aquaVersion(): String = remember {
    runCatching { AquaNative.nativeGetVersion() }.getOrNull() ?: "?"
}

@Preview(showBackground = true)
@Composable
private fun SettingsScreenPreview() {
    AquaTheme {
        SettingsScreen(remember { AquaController() })
    }
}
