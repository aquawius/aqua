package com.aquawius.aqua

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Intent
import android.content.pm.PackageManager
import android.os.Build
import android.os.IBinder
import androidx.core.app.NotificationCompat
import androidx.core.app.NotificationManagerCompat
import androidx.core.content.ContextCompat
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.delay
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch

/**
 * 前台服务：常驻通知栏播放控制（连接/断开），并防止后台播放与自动重连被系统冻结。
 *
 * - MainActivity 在 onCreate 注入 [controller]、启动服务；onDestroy 置空并停止。
 * - 通知动作经 PendingIntent.getService 回到本服务，在主线程调用 controller（与 UI 按钮一致）。
 */
class AquaService : Service() {

    companion object {
        const val CHANNEL_ID = "aqua_playback"
        const val NOTIFICATION_ID = 1
        const val ACTION_CONNECT = "com.aquawius.aqua.CONNECT"
        const val ACTION_DISCONNECT = "com.aquawius.aqua.DISCONNECT"

        /** MainActivity 注入的应用级 controller（主线程访问）。 */
        @Volatile
        var controller: AquaController? = null
    }

    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main)
    private var loopStarted = false

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onCreate() {
        super.onCreate()
        val channel = NotificationChannel(
            CHANNEL_ID,
            "播放控制",
            NotificationManager.IMPORTANCE_LOW,
        ).apply { description = "Aqua 连接状态与播放控制" }
        getSystemService(NotificationManager::class.java).createNotificationChannel(channel)
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        // startForegroundService 后必须尽快进入前台。
        startForeground(NOTIFICATION_ID, buildNotification())

        when (intent?.action) {
            ACTION_CONNECT -> controller?.connect()
            ACTION_DISCONNECT -> controller?.disconnect()
        }

        startUpdateLoop()
        return START_NOT_STICKY
    }

    /** 周期刷新通知内容（状态文案 / 动作按钮）。 */
    private fun startUpdateLoop() {
        if (loopStarted) return
        loopStarted = true
        scope.launch {
            while (isActive) {
                if (canPostNotifications()) {
                    NotificationManagerCompat.from(this@AquaService)
                        .notify(NOTIFICATION_ID, buildNotification())
                }
                delay(500)
            }
        }
    }

    private fun canPostNotifications(): Boolean =
        Build.VERSION.SDK_INT < 33 ||
            ContextCompat.checkSelfPermission(
                this,
                android.Manifest.permission.POST_NOTIFICATIONS,
            ) == PackageManager.PERMISSION_GRANTED

    private fun buildNotification(): Notification {
        val ctrl = controller
        val running = ctrl?.isRunning == true
        // 首次连接未成功即停止：显示"连接失败"而非"已停止"。
        val stateText = when {
            ctrl == null -> "未连接"
            ctrl.connectionFailed -> "连接失败"
            else -> ctrl.state.label
        }
        val text = if (running && ctrl != null) "$stateText · ${ctrl.serverIp}" else stateText

        val contentPi = PendingIntent.getActivity(
            this,
            0,
            Intent(this, MainActivity::class.java),
            PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT,
        )
        val connectPi = PendingIntent.getService(
            this,
            1,
            Intent(this, AquaService::class.java).setAction(ACTION_CONNECT),
            PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT,
        )
        val disconnectPi = PendingIntent.getService(
            this,
            2,
            Intent(this, AquaService::class.java).setAction(ACTION_DISCONNECT),
            PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT,
        )

        val builder = NotificationCompat.Builder(this, CHANNEL_ID)
            .setSmallIcon(R.drawable.ic_notification)
            .setContentTitle("Aqua")
            .setContentText(text)
            .setContentIntent(contentPi)
            .setOnlyAlertOnce(true)
            .setOngoing(true)

        if (running) {
            builder.addAction(R.drawable.ic_action_stop, "断开", disconnectPi)
        } else {
            builder.addAction(R.drawable.ic_action_play, "连接", connectPi)
        }
        return builder.build()
    }

    override fun onDestroy() {
        scope.cancel()
        super.onDestroy()
    }
}
