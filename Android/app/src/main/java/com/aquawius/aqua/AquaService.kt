package com.aquawius.aqua

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Intent
import android.content.pm.PackageManager
import android.media.AudioAttributes
import android.media.AudioFocusRequest
import android.media.AudioManager
import android.os.Build
import android.os.IBinder
import android.support.v4.media.MediaMetadataCompat
import android.support.v4.media.session.MediaSessionCompat
import android.support.v4.media.session.PlaybackStateCompat
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
 * 前台服务：MediaStyle 媒体通知（播放/断开）+ 音频焦点，防止后台播放与自动重连被系统冻结。
 *
 * - MainActivity 在 onCreate 注入 [controller]、启动服务；onDestroy 置空并停止。
 * - MediaSession 承载播放控制（onPlay/onPause/onStop -> connect/disconnect），
 *   通知与系统媒体面板（锁屏/音量键/蓝牙耳机按键）共用同一状态源。
 * - 音频焦点：播放期间持有 AUDIOFOCUS_GAIN（其他音乐 App 会自动暂停）；
 *   永久丢失（AUDIOFOCUS_LOSS，他方长期播放）时断开本端，瞬时丢失不打断。
 * - 通知动作经 MediaSession 回调走主线程调用 controller（与 UI 按钮一致）。
 */
class AquaService : Service() {

    companion object {
        const val CHANNEL_ID = "aqua_playback"
        const val NOTIFICATION_ID = 1

        /** MainActivity 注入的应用级 controller（主线程访问）。 */
        @Volatile
        var controller: AquaController? = null
    }

    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main)
    private var loopStarted = false

    // 上次已发出的通知内容；内容未变化时跳过重发，避免空转。
    private var lastNotifiedText: String? = null
    private var lastNotifiedRunning: Boolean? = null

    private lateinit var mediaSession: MediaSessionCompat

    // 音频焦点：持有标志（请求/释放只做一次，避免重复 requestAudioFocus 叠加）。
    private var holdingAudioFocus = false
    private val audioManager by lazy { getSystemService(AudioManager::class.java) }
    private val focusRequest: AudioFocusRequest? by lazy {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            AudioFocusRequest.Builder(AudioManager.AUDIOFOCUS_GAIN)
                .setAudioAttributes(
                    AudioAttributes.Builder()
                        .setUsage(AudioAttributes.USAGE_MEDIA)
                        .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC)
                        .build()
                )
                .setOnAudioFocusChangeListener(this::onAudioFocusChange)
                .build()
        } else {
            null
        }
    }

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onCreate() {
        super.onCreate()
        val channel = NotificationChannel(
            CHANNEL_ID,
            "播放控制",
            NotificationManager.IMPORTANCE_LOW,
        ).apply { description = "Aqua 连接状态与播放控制" }
        getSystemService(NotificationManager::class.java).createNotificationChannel(channel)
        initMediaSession()
    }

    private fun initMediaSession() {
        mediaSession = MediaSessionCompat(this, "AquaService")

        // 系统媒体面板点击 → 回到 App。
        mediaSession.setSessionActivity(
            PendingIntent.getActivity(
                this,
                0,
                Intent(this, MainActivity::class.java),
                PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT,
            )
        )

        // 播放控制：M1 的暂停语义即断开（恢复播放 = 重新连接）。
        mediaSession.setCallback(object : MediaSessionCompat.Callback() {
            override fun onPlay() {
                controller?.connect()
            }

            override fun onPause() {
                controller?.disconnect()
            }

            override fun onStop() {
                controller?.disconnect()
            }
        })
        mediaSession.isActive = true
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        // startForegroundService 后必须尽快进入前台。
        startForeground(NOTIFICATION_ID, buildNotification())

        // 通知动作按钮兜底入口（MediaSession 回调之外的第二条路径）。
        when (intent?.action) {
            "com.aquawius.aqua.CONNECT" -> controller?.connect()
            "com.aquawius.aqua.DISCONNECT" -> controller?.disconnect()
        }

        startUpdateLoop()
        return START_NOT_STICKY
    }

    /** 周期刷新通知内容（状态文案 / 播放态）；内容未变化时跳过重发。
     *  同时按运行状态持有/释放音频焦点。 */
    private fun startUpdateLoop() {
        if (loopStarted) return
        loopStarted = true
        scope.launch {
            while (isActive) {
                val running = controller?.isRunning == true
                updateAudioFocus(running)
                if (canPostNotifications()) {
                    postNotificationIfChanged()
                }
                delay(500)
            }
        }
    }

    /** 播放时持有 AUDIOFOCUS_GAIN（他方音乐 App 自动暂停）；停止后释放。 */
    private fun updateAudioFocus(running: Boolean) {
        if (running && !holdingAudioFocus) {
            holdingAudioFocus = true
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
                audioManager.requestAudioFocus(focusRequest!!)
            } else {
                @Suppress("DEPRECATION")
                audioManager.requestAudioFocus(
                    this::onAudioFocusChange,
                    AudioManager.STREAM_MUSIC,
                    AudioManager.AUDIOFOCUS_GAIN,
                )
            }
        } else if (!running && holdingAudioFocus) {
            holdingAudioFocus = false
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
                audioManager.abandonAudioFocusRequest(focusRequest!!)
            } else {
                @Suppress("DEPRECATION")
                audioManager.abandonAudioFocus(this::onAudioFocusChange)
            }
        }
    }

    /** 永久丢失（他方长期播放）→ 断开；瞬时丢失/闪避不打断流播放。 */
    private fun onAudioFocusChange(change: Int) {
        if (change == AudioManager.AUDIOFOCUS_LOSS) {
            holdingAudioFocus = false
            controller?.disconnect()
        }
    }

    private fun postNotificationIfChanged() {
        val ctrl = controller
        val running = ctrl?.isRunning == true
        val text = notificationText(ctrl, running)
        if (text == lastNotifiedText && running == lastNotifiedRunning) return
        lastNotifiedText = text
        lastNotifiedRunning = running
        updatePlaybackState(running)
        updateMetadata(ctrl, running)
        NotificationManagerCompat.from(this).notify(NOTIFICATION_ID, buildNotification())
    }

    /** 媒体会话元数据：系统媒体面板 / 锁屏 / 蓝牙耳机按键显示曲目信息。
     *  单音频流没有"上下曲"，标题用服务器地址、副标题用连接状态。 */
    private fun updateMetadata(ctrl: AquaController?, running: Boolean) {
        val title = if (running && ctrl != null) ctrl.serverIp else "Aqua"
        val subtitle = if (running && ctrl != null) ctrl.state.label else "未连接"
        mediaSession.setMetadata(
            MediaMetadataCompat.Builder()
                .putString(MediaMetadataCompat.METADATA_KEY_TITLE, title)
                .putString(MediaMetadataCompat.METADATA_KEY_ARTIST, "Aqua 音频流共享")
                .putString(MediaMetadataCompat.METADATA_KEY_ALBUM, subtitle)
                .build()
        )
    }

    /** MediaSession 播放态与可用动作（通知按钮/系统媒体面板共用）。 */
    private fun updatePlaybackState(running: Boolean) {
        val state = if (running) {
            PlaybackStateCompat.STATE_PLAYING
        } else {
            PlaybackStateCompat.STATE_PAUSED
        }
        val actions = if (running) {
            PlaybackStateCompat.ACTION_PAUSE or PlaybackStateCompat.ACTION_STOP
        } else {
            PlaybackStateCompat.ACTION_PLAY
        }
        mediaSession.setPlaybackState(
            PlaybackStateCompat.Builder()
                .setActions(actions)
                .setState(state, PlaybackStateCompat.PLAYBACK_POSITION_UNKNOWN, 0f)
                .build()
        )
    }

    private fun notificationText(ctrl: AquaController?, running: Boolean): String {
        val stateText = when {
            ctrl == null -> "未连接"
            ctrl.connectionFailed -> "连接失败"
            else -> ctrl.state.label
        }
        return if (running && ctrl != null) "$stateText · ${ctrl.serverIp}" else stateText
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
        val text = notificationText(ctrl, running)

        val contentPi = PendingIntent.getActivity(
            this,
            0,
            Intent(this, MainActivity::class.java),
            PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT,
        )

        val builder = NotificationCompat.Builder(this, CHANNEL_ID)
            .setSmallIcon(R.drawable.ic_notification)
            .setContentTitle("Aqua")
            .setContentText(text)
            .setContentIntent(contentPi)
            .setOnlyAlertOnce(true)
            .setOngoing(running)
            .setVisibility(NotificationCompat.VISIBILITY_PUBLIC)

        // MediaStyle：系统按媒体通知渲染（锁屏/媒体面板/耳机按键统一入口）。
        // 动作经 MediaSession 回调派发（onPlay/onPause），不再单独挂 PendingIntent。
        builder.setStyle(
            androidx.media.app.NotificationCompat.MediaStyle()
                .setMediaSession(mediaSession.sessionToken)
                .setShowActionsInCompactView(0)
        )

        // 兼容无媒体面板渲染的旧设备：直接挂通知动作按钮。
        if (running) {
            builder.addAction(
                R.drawable.ic_action_pause, "断开",
                servicePendingIntent("com.aquawius.aqua.DISCONNECT"),
            )
        } else {
            builder.addAction(
                R.drawable.ic_action_play, "连接",
                servicePendingIntent("com.aquawius.aqua.CONNECT"),
            )
        }
        return builder.build()
    }

    /** MediaSession 之外的兜底动作入口（旧系统通知按钮）。 */
    private fun servicePendingIntent(action: String): PendingIntent =
        PendingIntent.getService(
            this,
            action.hashCode(),
            Intent(this, AquaService::class.java).setAction(action),
            PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT,
        )

    override fun onDestroy() {
        updateAudioFocus(running = false)
        stopForeground(STOP_FOREGROUND_REMOVE)
        mediaSession.release()
        scope.cancel()
        super.onDestroy()
    }
}
