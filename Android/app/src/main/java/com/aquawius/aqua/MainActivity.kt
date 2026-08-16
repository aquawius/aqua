package com.aquawius.aqua

import android.content.SharedPreferences
import android.os.Bundle
import android.view.WindowManager
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.GraphicEq
import androidx.compose.material.icons.filled.Settings
import androidx.compose.material.icons.filled.Tune
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Icon
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
import com.aquawius.aqua.ui.AdvancedScreen
import com.aquawius.aqua.ui.AquaScreen
import com.aquawius.aqua.ui.SettingsScreen
import com.aquawius.aqua.ui.theme.AquaTheme
import kotlinx.coroutines.delay

private enum class AquaTab(val label: String, val icon: ImageVector) {
    Home("Aqua", Icons.Filled.GraphicEq),
    Advanced("高级", Icons.Filled.Tune),
    Settings("设置", Icons.Filled.Settings),
}

@OptIn(ExperimentalMaterial3Api::class)
class MainActivity : ComponentActivity() {
    private lateinit var controller: AquaController

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        enableEdgeToEdge()

        val prefs: SharedPreferences = getSharedPreferences("aqua", MODE_PRIVATE)
        val savedIp = prefs.getString(KEY_SERVER_IP, null)

        controller = AquaController(
            initialServerIp = savedIp ?: "192.168.1.100",
            onConnected = { ip ->
                prefs.edit().putString(KEY_SERVER_IP, ip).apply()
            },
        )

        setContent {
            AquaTheme {
                var selectedTab by remember { mutableStateOf(AquaTab.Home) }

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

                Scaffold(
                    modifier = Modifier.fillMaxSize(),
                    topBar = {
                        TopAppBar(title = { Text(selectedTab.label) })
                    },
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
                    when (selectedTab) {
                        AquaTab.Home -> AquaScreen(controller, contentModifier)
                        AquaTab.Advanced -> AdvancedScreen(controller, contentModifier)
                        AquaTab.Settings -> SettingsScreen(controller, contentModifier)
                    }
                }
            }
        }
    }

    override fun onDestroy() {
        if (::controller.isInitialized) {
            controller.destroy()
        }
        super.onDestroy()
    }

    companion object {
        private const val KEY_SERVER_IP = "server_ip"
    }
}
