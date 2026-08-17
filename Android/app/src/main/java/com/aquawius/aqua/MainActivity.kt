package com.aquawius.aqua

import android.Manifest
import android.content.Intent
import android.content.SharedPreferences
import android.content.pm.PackageManager
import android.os.Build
import android.os.Bundle
import android.provider.Settings as SystemSettings
import android.view.WindowManager
import androidx.activity.compose.BackHandler
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.animation.AnimatedContent
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.animation.slideInHorizontally
import androidx.compose.animation.slideOutHorizontally
import androidx.compose.animation.togetherWith
import androidx.compose.animation.core.tween
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
import androidx.compose.runtime.saveable.rememberSaveableStateHolder
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.core.content.ContextCompat
import com.aquawius.aqua.ui.AboutScreen
import com.aquawius.aqua.ui.AdvancedScreen
import com.aquawius.aqua.ui.AquaScreen
import com.aquawius.aqua.ui.SettingsScreen
import com.aquawius.aqua.ui.theme.AquaTheme
import com.aquawius.aqua.ui.theme.AquaThemeMode
import com.aquawius.aqua.ui.theme.AquaThemeStyle
import kotlinx.coroutines.delay

private enum class AquaTab(val label: String, val icon: ImageVector) {
    Home("Aqua", Icons.Filled.GraphicEq),
    Advanced("高级", Icons.Filled.Tune),
    Settings("设置", Icons.Filled.Settings),
}

/** 页面路由：底部导航三页 + 覆盖式关于页，用于切换动画方向判定。 */
private sealed interface Screen {
    data class Tab(val tab: AquaTab) : Screen
    data object About : Screen
}

private fun screenOrder(s: Screen): Int = when (s) {
    is Screen.Tab -> s.tab.ordinal
    Screen.About -> AquaTab.entries.size
}

/** SaveableStateHolder 的页面 key（与 Screen 一一对应）。 */
private fun keyOf(s: Screen): String = when (s) {
    is Screen.Tab -> "tab_${s.tab.name}"
    Screen.About -> "about"
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
            initialJitterBufferMs = prefs.getInt(KEY_JITTER_BUFFER_MS, 0),
            initialJitterDetectWindowPackets = prefs.getInt(KEY_JITTER_DETECT_WINDOW, 0),
            initialPlaybackBufferKb = prefs.getInt(KEY_PLAYBACK_BUFFER_KB, 0),
            initialClientName = prefs.getString(KEY_CLIENT_NAME, null) ?: deviceDisplayName(),
            onConnected = { c ->
                // 成功进入播放态：持久化连接与高级参数。
                prefs.edit()
                    .putString(KEY_SERVER_IP, c.serverIp.trim())
                    .putInt(KEY_JITTER_BUFFER_MS, c.jitterBufferMs)
                    .putInt(KEY_JITTER_DETECT_WINDOW, c.jitterDetectWindowPackets)
                    .putInt(KEY_PLAYBACK_BUFFER_KB, c.playbackBufferKb)
                    .putString(KEY_CLIENT_NAME, c.clientName.trim())
                    .apply()
            },
        )

        // 连接前置：请求通知授权 + 启动前台服务（首次点击"连接"时才触发，不打扰启动）。
        controller.onConnectRequested = {
            requestNotificationPermissionIfNeeded()
            AquaService.controller = controller
            ContextCompat.startForegroundService(this, Intent(this, AquaService::class.java))
        }

        setContent {
            val savedStyle = remember {
                prefs.getString(KEY_THEME_STYLE, null)
                    ?.let { name -> AquaThemeStyle.entries.firstOrNull { it.name == name } }
            } ?: AquaThemeStyle.AQUA
            val savedMode = remember {
                prefs.getString(KEY_THEME_MODE, null)
                    ?.let { name -> AquaThemeMode.entries.firstOrNull { it.name == name } }
            } ?: AquaThemeMode.MATERIAL
            var themeStyle by remember { mutableStateOf(savedStyle) }
            var themeMode by remember { mutableStateOf(savedMode) }
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
                    val screen: Screen = if (showAbout) Screen.About else Screen.Tab(selectedTab)

                    // 页面切换过渡：按导航方向水平滑动 + 淡入淡出。
                    // SaveableStateHolder 按页面 key 保存/恢复组合内 saveable 状态
                    //（rememberScrollState / rememberLazyListState），切换 Tab 或
                    // 关于页返回后滚动位置不丢失。
                    val stateHolder = rememberSaveableStateHolder()
                    AnimatedContent(
                        targetState = screen,
                        transitionSpec = {
                            val forward = screenOrder(targetState) >= screenOrder(initialState)
                            val dir = if (forward) 1 else -1
                            (slideInHorizontally(tween(220)) { dir * it / 3 } +
                                fadeIn(tween(220))) togetherWith
                                (slideOutHorizontally(tween(200)) { -dir * it / 3 } +
                                    fadeOut(tween(160)))
                        },
                        label = "screen",
                    ) { target ->
                        val contentModifier = Modifier.padding(innerPadding)
                        stateHolder.SaveableStateProvider(keyOf(target)) {
                            when (target) {
                                Screen.About -> AboutScreen(contentModifier)
                                is Screen.Tab -> when (target.tab) {
                                    AquaTab.Home -> AquaScreen(controller, contentModifier)
                                    AquaTab.Advanced -> AdvancedScreen(controller, contentModifier)
                                    AquaTab.Settings -> SettingsScreen(
                                        controller = controller,
                                        themeStyle = themeStyle,
                                        onThemeStyleChange = { style ->
                                            themeStyle = style
                                            prefs.edit().putString(KEY_THEME_STYLE, style.name).apply()
                                        },
                                        themeMode = themeMode,
                                        onThemeModeChange = { mode ->
                                            themeMode = mode
                                            prefs.edit().putString(KEY_THEME_MODE, mode.name).apply()
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
        }
    }

    /** 手机设备名（设置里用户可改的名字）；拿不到时回归型号/默认名。 */
    private fun deviceDisplayName(): String =
        runCatching {
            SystemSettings.Global.getString(contentResolver, SystemSettings.Global.DEVICE_NAME)
        }.getOrNull()?.takeIf { it.isNotBlank() && it != Build.MODEL }
            ?: Build.MODEL.takeIf { it.isNotBlank() }
            ?: "aqua_android"

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
        private const val KEY_JITTER_BUFFER_MS = "jitter_buffer_ms"
        private const val KEY_JITTER_DETECT_WINDOW = "jitter_detect_window_packets"
        private const val KEY_PLAYBACK_BUFFER_KB = "playback_buffer_kb"
        private const val KEY_CLIENT_NAME = "client_name"
        private const val KEY_THEME_STYLE = "theme_style"
        private const val KEY_THEME_MODE = "theme_mode"
    }
}
