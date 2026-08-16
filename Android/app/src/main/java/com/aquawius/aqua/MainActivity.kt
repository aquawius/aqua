package com.aquawius.aqua

import android.Manifest
import android.content.Intent
import android.content.SharedPreferences
import android.content.pm.PackageManager
import android.os.Build
import android.os.Bundle
import android.view.WindowManager
import androidx.activity.compose.BackHandler
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material.icons.filled.GraphicEq
import androidx.compose.material.icons.filled.Settings
import androidx.compose.material.icons.filled.Tune
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.NavigationBar
import androidx.compose.material3.NavigationBarItem
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.material3.TopAppBar
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.core.content.ContextCompat
import com.aquawius.aqua.ui.AboutScreen
import com.aquawius.aqua.ui.AdvancedScreen
import com.aquawius.aqua.ui.AquaScreen
import com.aquawius.aqua.ui.SettingsScreen
import com.aquawius.aqua.ui.theme.AquaTheme
import com.aquawius.aqua.ui.theme.AquaThemeStyle
import kotlinx.coroutines.delay

private enum class AquaTab(val label: String, val icon: ImageVector) {
    Home("Aqua", Icons.Filled.GraphicEq),
    Advanced("高级", Icons.Filled.Tune),
    Settings("设置", Icons.Filled.Settings),
}

@OptIn(ExperimentalMaterial3Api::class)
class MainActivity : ComponentActivity() {
    private lateinit var controller: AquaController

    private val notificationPermissionLauncher =
        registerForActivityResult(ActivityResultContracts.RequestPermission()) { }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        enableEdgeToEdge()

        val prefs: SharedPreferences = getSharedPreferences("aqua", MODE_PRIVATE)

        controller = AquaController(
            initialServerIp = prefs.getString(KEY_SERVER_IP, null) ?: "192.168.1.100",
            initialJitterLatencyMs = prefs.getInt(KEY_JITTER_MS, 0),
            initialDriftThreshold = prefs.getInt(KEY_DRIFT_THRESHOLD, 0),
            initialPlaybackBufferKb = prefs.getInt(KEY_PLAYBACK_BUFFER_KB, 0),
            initialClientName = prefs.getString(KEY_CLIENT_NAME, null) ?: "aqua_android",
            onConnected = { c ->
                // 成功进入播放态：持久化连接与高级参数。
                prefs.edit()
                    .putString(KEY_SERVER_IP, c.serverIp.trim())
                    .putInt(KEY_JITTER_MS, c.jitterLatencyMs)
                    .putInt(KEY_DRIFT_THRESHOLD, c.driftThreshold)
                    .putInt(KEY_PLAYBACK_BUFFER_KB, c.playbackBufferKb)
                    .putString(KEY_CLIENT_NAME, c.clientName.trim())
                    .apply()
            },
        )

        // 前台服务：通知栏播放控制 + 后台保活。
        requestNotificationPermissionIfNeeded()
        AquaService.controller = controller
        ContextCompat.startForegroundService(this, Intent(this, AquaService::class.java))

        setContent {
            val savedStyle = remember {
                prefs.getString(KEY_THEME_STYLE, null)
                    ?.let { name -> AquaThemeStyle.entries.firstOrNull { it.name == name } }
            } ?: AquaThemeStyle.AQUA
            var themeStyle by remember { mutableStateOf(savedStyle) }
            var selectedTab by remember { mutableStateOf(AquaTab.Home) }
            var showAbout by remember { mutableStateOf(false) }

            AquaTheme(style = themeStyle) {
                // 轮询：每 250ms 拉取 state/lastError/diagnostics。
                LaunchedEffect(Unit) {
                    while (true) {
                        controller.poll()
                        delay(250)
                    }
                }

                // 播放时屏幕常亮（设置开启且正在播放）。
                LaunchedEffect(controller.keepScreenOn, controller.isRunning) {
                    if (controller.keepScreenOn && controller.isRunning) {
                        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
                    } else {
                        window.clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
                    }
                }

                // 关于页返回键。
                BackHandler(enabled = showAbout) { showAbout = false }

                Scaffold(
                    modifier = Modifier.fillMaxSize(),
                    topBar = {
                        if (showAbout) {
                            TopAppBar(
                                title = { Text("关于") },
                                navigationIcon = {
                                    IconButton(onClick = { showAbout = false }) {
                                        Icon(
                                            Icons.AutoMirrored.Filled.ArrowBack,
                                            contentDescription = "返回",
                                        )
                                    }
                                },
                            )
                        } else {
                            TopAppBar(title = { Text(selectedTab.label) })
                        }
                    },
                    bottomBar = {
                        if (!showAbout) {
                            NavigationBar {
                                AquaTab.entries.forEach { tab ->
                                    NavigationBarItem(
                                        selected = selectedTab == tab,
                                        onClick = { selectedTab = tab },
                                        icon = { Icon(tab.icon, contentDescription = tab.label) },
                                        label = { Text(tab.label) },
                                    )
                                }
                            }
                        }
                    },
                ) { innerPadding ->
                    val contentModifier = Modifier.padding(innerPadding)
                    if (showAbout) {
                        AboutScreen(contentModifier)
                    } else {
                        when (selectedTab) {
                            AquaTab.Home -> AquaScreen(controller, contentModifier)
                            AquaTab.Advanced -> AdvancedScreen(controller, contentModifier)
                            AquaTab.Settings -> SettingsScreen(
                                controller = controller,
                                themeStyle = themeStyle,
                                onThemeStyleChange = { style ->
                                    themeStyle = style
                                    prefs.edit().putString(KEY_THEME_STYLE, style.name).apply()
                                },
                                onAboutClick = { showAbout = true },
                                modifier = contentModifier,
                            )
                        }
                    }
                }
            }
        }
    }

    private fun requestNotificationPermissionIfNeeded() {
        if (Build.VERSION.SDK_INT >= 33 &&
            ContextCompat.checkSelfPermission(this, Manifest.permission.POST_NOTIFICATIONS) !=
            PackageManager.PERMISSION_GRANTED
        ) {
            notificationPermissionLauncher.launch(Manifest.permission.POST_NOTIFICATIONS)
        }
    }

    override fun onDestroy() {
        AquaService.controller = null
        stopService(Intent(this, AquaService::class.java))
        if (::controller.isInitialized) {
            controller.destroy()
        }
        super.onDestroy()
    }

    companion object {
        private const val KEY_SERVER_IP = "server_ip"
        private const val KEY_JITTER_MS = "jitter_latency_ms"
        private const val KEY_DRIFT_THRESHOLD = "drift_threshold"
        private const val KEY_PLAYBACK_BUFFER_KB = "playback_buffer_kb"
        private const val KEY_CLIENT_NAME = "client_name"
        private const val KEY_THEME_STYLE = "theme_style"
    }
}
