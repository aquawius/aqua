package com.aquawius.aqua.ui

import android.content.Intent
import android.net.Uri
import android.os.Build
import android.provider.Settings
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Card
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp
import com.aquawius.aqua.AquaController
import com.aquawius.aqua.native.AquaNative

/** 设置：自动重连 / 屏幕常亮 / 忽略电池优化 + 关于。 */
@Composable
fun SettingsScreen(controller: AquaController, modifier: Modifier = Modifier) {
    val context = LocalContext.current

    Column(
        modifier = modifier
            .fillMaxSize()
            .verticalScroll(rememberScrollState())
            .padding(16.dp),
        verticalArrangement = Arrangement.spacedBy(8.dp),
    ) {
        SettingSwitch(
            title = "自动重连",
            subtitle = "断线后指数退避重连",
            checked = controller.autoReconnect,
            onCheckedChange = { controller.autoReconnect = it },
        )
        SettingSwitch(
            title = "播放时屏幕常亮",
            subtitle = "播放期间保持屏幕常亮",
            checked = controller.keepScreenOn,
            onCheckedChange = { controller.keepScreenOn = it },
        )
        SettingSwitch(
            title = "忽略电池优化",
            subtitle = "请求系统忽略电池优化（弹出系统确认）",
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

        AboutCard(context)
    }
}

@Composable
private fun SettingSwitch(
    title: String,
    subtitle: String,
    checked: Boolean,
    onCheckedChange: (Boolean) -> Unit,
) {
    Card(modifier = Modifier.fillMaxWidth()) {
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .padding(16.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Column(Modifier.weight(1f)) {
                Text(title, style = androidx.compose.material3.MaterialTheme.typography.titleMedium)
                Text(subtitle)
            }
            Switch(checked = checked, onCheckedChange = onCheckedChange)
        }
    }
}

@Composable
private fun AboutCard(context: android.content.Context) {
    val appVersion = remember {
        runCatching {
            context.packageManager.getPackageInfo(context.packageName, 0).versionName
        }.getOrNull() ?: "?"
    }
    val aquaVersion = remember {
        runCatching { AquaNative.nativeGetVersion() }.getOrNull() ?: "?"
    }

    Card(modifier = Modifier.fillMaxWidth()) {
        Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(4.dp)) {
            Text("关于", style = androidx.compose.material3.MaterialTheme.typography.titleMedium)
            AboutRow("设备", "${Build.MANUFACTURER} ${Build.MODEL}")
            AboutRow("Android", "${Build.VERSION.RELEASE} (API ${Build.VERSION.SDK_INT})")
            AboutRow("App 版本", appVersion)
            AboutRow("Aqua 库版本", aquaVersion)
        }
    }
}

@Composable
private fun AboutRow(label: String, value: String) {
    Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween) {
        Text(label)
        Text(value)
    }
}
