package com.aquawius.aqua.ui

import android.os.Build
import androidx.compose.foundation.Image
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.ListItem
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedCard
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalInspectionMode
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.tooling.preview.Preview
import androidx.compose.ui.unit.dp
import com.aquawius.aqua.R
import com.aquawius.aqua.native.AquaNative
import com.aquawius.aqua.ui.theme.AquaTheme

/** 关于页：应用标识 + 版本/设备信息。 */
@Composable
fun AboutScreen(modifier: Modifier = Modifier) {
    val context = androidx.compose.ui.platform.LocalContext.current

    Column(
        modifier = modifier
            .fillMaxSize()
            .verticalScroll(rememberScrollState())
            .padding(24.dp),
        horizontalAlignment = Alignment.CenterHorizontally,
        verticalArrangement = Arrangement.spacedBy(16.dp),
    ) {
        Spacer(Modifier.height(32.dp))

        Image(
            painter = painterResource(R.drawable.ic_launcher_foreground),
            contentDescription = null,
            modifier = Modifier.size(96.dp),
        )
        Text(
            "Aqua",
            style = MaterialTheme.typography.headlineMedium,
            fontWeight = FontWeight.Bold,
        )
        Text(
            "音频流共享客户端",
            style = MaterialTheme.typography.bodyMedium,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )

        Spacer(Modifier.height(8.dp))

        OutlinedCard(Modifier.fillMaxWidth()) {
            Column {
                AboutRow("App 版本", appVersion(context))
                InsetDivider()
                AboutRow("Aqua 库版本", aquaVersion())
                InsetDivider()
                AboutRow("设备", "${Build.MANUFACTURER} ${Build.MODEL}")
                InsetDivider()
                AboutRow("Android", "${Build.VERSION.RELEASE} (API ${Build.VERSION.SDK_INT})")
            }
        }
    }
}

@Composable
private fun InsetDivider() {
    HorizontalDivider(modifier = Modifier.padding(start = 16.dp))
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
private fun appVersion(context: android.content.Context): String {
    // 预览环境（layoutlib）拿不到 PackageManager，给静态占位。
    if (LocalInspectionMode.current) return "preview"
    return remember {
        runCatching {
            context.packageManager.getPackageInfo(context.packageName, 0).versionName
        }.getOrNull() ?: "?"
    }
}

@Composable
private fun aquaVersion(): String {
    // 预览环境运行在宿主 JVM，没有 libaqua.so，禁止触发 System.loadLibrary。
    if (LocalInspectionMode.current) return "preview"
    return remember {
        runCatching { AquaNative.nativeGetVersion() }.getOrNull() ?: "?"
    }
}

@Preview(showBackground = true)
@Composable
private fun AboutScreenPreview() {
    AquaTheme {
        AboutScreen()
    }
}
