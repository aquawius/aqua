package com.aquawius.aqua

import android.Manifest
import android.content.Intent
import android.content.SharedPreferences
import android.content.pm.PackageManager
import android.media.AudioManager
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
import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.animation.slideInHorizontally
import androidx.compose.animation.slideOutHorizontally
import androidx.compose.animation.togetherWith
import androidx.compose.animation.core.tween
import androidx.compose.foundation.layout.Box
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
import kotlin.time.Duration.Companion.milliseconds
import androidx.core.content.edit

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

        // 进程级持有 controller：未声明的配置变更（如 locale）重建 Activity 时，
        // 播放会话不丢（新 Activity 复用同一 controller，服务/轮询无缝衔接）。
        controller = retainedController ?: AquaController(
            initialServerIp = prefs.getString(KEY_SERVER_IP, null) ?: "192.168.1.100",
            initialJitterBufferSlots = prefs.getInt(KEY_JITTER_BUFFER_SLOTS, 0),
            initialHelloIntervalMs = prefs.getInt(KEY_HELLO_INTERVAL_MS, 0),
            initialClientName = prefs.getString(KEY_CLIENT_NAME, null) ?: deviceDisplayName(),
            initialForceUdpPort = prefs.getString(KEY_FORCE_UDP_PORT, "") ?: "",
            initialLogLevel = prefs.getInt(KEY_LOG_LEVEL, -1),
            initialAutoReconnect = prefs.getBoolean(KEY_AUTO_RECONNECT, false),
            initialKeepScreenOn = prefs.getBoolean(KEY_KEEP_SCREEN_ON, false),
            initialAllowSimultaneousPlayback =
            prefs.getBoolean(KEY_ALLOW_SIMULTANEOUS, false),
            initialPlaybackLowLatency = prefs.getBoolean(KEY_PLAYBACK_LOW_LATENCY, true),
            initialAutoSwitchPlaybackDevice =
            prefs.getBoolean(KEY_AUTO_SWITCH_PLAYBACK_DEVICE, true),
            onConnected = { c ->
                // 成功进入播放态：持久化连接与高级参数。
                prefs.edit {
                    putString(KEY_SERVER_IP, c.serverIp.trim())
                        .putInt(KEY_JITTER_BUFFER_SLOTS, c.jitterBufferSlots)
                        .putInt(KEY_HELLO_INTERVAL_MS, c.helloIntervalMs)
                        .putString(KEY_CLIENT_NAME, c.clientName.trim())
                        .putString(KEY_FORCE_UDP_PORT, c.forceUdpPort.trim())
                        .putInt(KEY_LOG_LEVEL, c.logLevel)
                }
            },
        ).also { retainedController = it }

        // 播放设备监视器：进程级持有（与 retainedController 同生命周期），
        // App 启动即推送设备快照——未连接时设备弹层就有列表（此前由
        // AquaService 持有，而服务首次连接才启动，导致未连接看不到设备）。
        // 配置变更重建 Activity 不重启（onDestroy 仅 isFinishing 时停止）；
        // 后台播放期间 Activity 仅 onStop 不销毁，回调持续有效。
        if (retainedDeviceMonitor == null) {
            retainedDeviceMonitor = AudioDeviceMonitor(
                getSystemService(AudioManager::class.java),
            ).apply {
                onDevicesChanged = { devices ->
                    retainedController?.updatePlaybackDevices(devices)
                }
                start()
            }
        }

        // 每次重建都重绑（回调捕获当前 activity 实例，用于权限请求/服务启动）。
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

            // 页面状态持有者放在 Scaffold 之外：切换"关于"页会改变 Scaffold 的
            // topBar/bottomBar（进而 innerPadding），放这里避免其被重建而丢失滚动位置。
            val stateHolder = rememberSaveableStateHolder()

            AquaTheme(style = themeStyle) {
                // 轮询：每 500ms 拉取 state/lastError（诊断再经 Controller 节流）。
                LaunchedEffect(Unit) {
                    while (true) {
                        controller.poll()
                        delay(500.milliseconds)
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

            Box(Modifier.fillMaxSize()) {
                // 主 Scaffold：topBar/bottomBar 恒定（tab 标题 + 底部导航），
                // About 页为全屏覆盖层，不再随 showAbout 切换 topBar/bottomBar。
                // 这样 innerPadding 稳定、tab 内容 viewport 不变，滚动位置不会被钳制。
                Scaffold(
                    modifier = Modifier.fillMaxSize(),
                    topBar = { TopAppBar(title = { Text(selectedTab.label) }) },
                    bottomBar = {
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
                    },
                ) { innerPadding ->
                    val contentModifier = Modifier.padding(innerPadding)

                    // Tab 切换过渡：按导航方向水平滑动 + 淡入淡出。
                    // SaveableStateHolder 按 tab key 保存/恢复滚动位置。
                    AnimatedContent(
                        targetState = selectedTab,
                        transitionSpec = {
                            val forward = targetState.ordinal >= initialState.ordinal
                            val dir = if (forward) 1 else -1
                            (slideInHorizontally(tween(220)) { dir * it / 3 } +
                                    fadeIn(tween(220))) togetherWith
                                    (slideOutHorizontally(tween(200)) { -dir * it / 3 } +
                                            fadeOut(tween(160)))
                        },
                        label = "tab",
                    ) { tab ->
                        stateHolder.SaveableStateProvider("tab_${tab.name}") {
                            when (tab) {
                                AquaTab.Home -> AquaScreen(controller, contentModifier)
                                AquaTab.Advanced -> AdvancedScreen(controller, contentModifier)
                                AquaTab.Settings -> SettingsScreen(
                                    controller = controller,
                                    themeStyle = themeStyle,
                                    onThemeStyleChange = { style ->
                                        themeStyle = style
                                        prefs.edit { putString(KEY_THEME_STYLE, style.name) }
                                    },
                                    themeMode = themeMode,
                                    onThemeModeChange = { mode ->
                                        themeMode = mode
                                        prefs.edit { putString(KEY_THEME_MODE, mode.name) }
                                    },
                                    onAboutClick = { showAbout = true },
                                    modifier = contentModifier,
                                )
                            }
                        }
                    }
                }

                // About 全屏覆盖层：覆盖主 Scaffold（含 topBar/bottomBar）。
                AnimatedVisibility(
                    visible = showAbout,
                    enter = slideInHorizontally(tween(220)) { it } + fadeIn(tween(220)),
                    exit = slideOutHorizontally(tween(200)) { it } + fadeOut(tween(160)),
                    modifier = Modifier.fillMaxSize(),
                ) {
                    Scaffold(
                        modifier = Modifier.fillMaxSize(),
                        topBar = {
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
                        },
                    ) { aboutPadding ->
                        AboutScreen(Modifier.padding(aboutPadding))
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

    override fun onStop() {
        super.onStop()
        // 设置开关持久化（连接参数在成功播放时另行保存）。
        if (::controller.isInitialized) {
            getSharedPreferences("aqua", MODE_PRIVATE).edit()
                .putBoolean(KEY_AUTO_RECONNECT, controller.autoReconnect)
                .putBoolean(KEY_KEEP_SCREEN_ON, controller.keepScreenOn)
                .putBoolean(KEY_ALLOW_SIMULTANEOUS, controller.allowSimultaneousPlayback)
                .putBoolean(KEY_PLAYBACK_LOW_LATENCY, controller.playbackLowLatency)
                .putBoolean(KEY_AUTO_SWITCH_PLAYBACK_DEVICE, controller.autoSwitchPlaybackDevice)
                .apply()
        }
    }

    override fun onDestroy() {
        // 仅真正退出时 teardown；配置变更重建（清单未声明的如 locale）
        // 会走 onDestroy 且 isFinishing=false，此时不能误杀后台播放。
        if (isFinishing) {
            retainedController = null
            retainedDeviceMonitor?.stop()
            retainedDeviceMonitor = null
            AquaService.controller = null
            stopService(Intent(this, AquaService::class.java))
            if (::controller.isInitialized) {
                controller.destroy()
            }
        }
        super.onDestroy()
    }

    companion object {
        /** 进程级 controller 持有：Activity 重建（配置变更）复用同一会话。 */
        @Volatile
        private var retainedController: AquaController? = null

        /** 进程级播放设备监视器：App 启动即工作（设备列表不依赖连接）。 */
        @Volatile
        private var retainedDeviceMonitor: AudioDeviceMonitor? = null

        private const val KEY_SERVER_IP = "server_ip"
        private const val KEY_JITTER_BUFFER_SLOTS = "jitter_buffer_slots"
        private const val KEY_HELLO_INTERVAL_MS = "hello_interval_ms"
        private const val KEY_CLIENT_NAME = "client_name"
        private const val KEY_FORCE_UDP_PORT = "force_udp_port"
        private const val KEY_LOG_LEVEL = "log_level"
        private const val KEY_AUTO_RECONNECT = "auto_reconnect"
        private const val KEY_KEEP_SCREEN_ON = "keep_screen_on"
        private const val KEY_ALLOW_SIMULTANEOUS = "allow_simultaneous_playback"
        private const val KEY_PLAYBACK_LOW_LATENCY = "playback_low_latency"
        private const val KEY_AUTO_SWITCH_PLAYBACK_DEVICE = "auto_switch_playback_device"
        private const val KEY_THEME_STYLE = "theme_style"
        private const val KEY_THEME_MODE = "theme_mode"
    }
}
