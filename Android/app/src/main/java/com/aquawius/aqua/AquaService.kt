package com.aquawius.aqua

import android.annotation.SuppressLint
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
import kotlin.time.Duration.Companion.milliseconds

/**
 * 前台服务：标准音乐播放器式 MediaStyle 媒体通知（上一曲 / 播放暂停 / 下一曲）+ 音频焦点，
 * 防止后台播放与自动重连被系统冻结。
 *
 * - MainActivity 在 onCreate 注入 [controller]、启动服务；onDestroy 置空并停止。
 * - MediaSession 承载播放控制（onPlay/onPause/onStop -> connect/disconnect，
 *   onSkipToNext/onSkipToPrevious -> restart），通知三键与系统媒体面板
 *  （锁屏 / 音量键 / 蓝牙耳机按键）共用同一状态源。
 * - 音频焦点：播放期间持有 AUDIOFOCUS_GAIN（其他音乐 App 会自动暂停）；
 *   永久丢失（AUDIOFOCUS_LOSS，他方长期播放）时断开本端，瞬时丢失不打断。
 * - 通知三键经 PendingIntent → onStartCommand 走主线程调用 controller；系统媒体面板
 *   按键经 MediaSession 回调走主线程，两条路径最终一致。
 */
class AquaService : Service() {

    companion object {
        const val CHANNEL_ID = "aqua_playback"
        const val NOTIFICATION_ID = 1

        private const val ACTION_CONNECT = "com.aquawius.aqua.CONNECT"
        private const val ACTION_DISCONNECT = "com.aquawius.aqua.DISCONNECT"
        private const val ACTION_SKIP_PREVIOUS = "com.aquawius.aqua.SKIP_PREVIOUS"
        private const val ACTION_SKIP_NEXT = "com.aquawius.aqua.SKIP_NEXT"

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
        AudioFocusRequest.Builder(AudioManager.AUDIOFOCUS_GAIN)
            .setAudioAttributes(
                AudioAttributes.Builder()
                    .setUsage(AudioAttributes.USAGE_MEDIA)
                    .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC)
                    .build()
            )
            .setOnAudioFocusChangeListener(this::onAudioFocusChange)
            .build()
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

        // 播放控制：暂停/停止语义即断开（恢复播放 = 重新连接）。
        // 上下曲：单流无曲目队列，映射为"重新同步"（断开并重连），从卡顿/失步恢复。
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

            override fun onSkipToNext() {
                controller?.restart()
            }

            override fun onSkipToPrevious() {
                controller?.restart()
            }
        })
        mediaSession.isActive = true
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        // startForegroundService 后必须尽快进入前台。
        startForeground(NOTIFICATION_ID, buildNotification())

        // 通知栏三键媒体控件的动作入口（PendingIntent 路径；系统媒体面板走 MediaSession 回调）。
        when (intent?.action) {
            ACTION_CONNECT -> controller?.connect()
            ACTION_DISCONNECT -> controller?.disconnect()
            ACTION_SKIP_PREVIOUS, ACTION_SKIP_NEXT -> controller?.restart()
        }

        startUpdateLoop()
        return START_NOT_STICKY
    }

    /** 周期刷新：按运行状态持有/释放音频焦点，并同步 MediaSession 与通知。 */
    private fun startUpdateLoop() {
        if (loopStarted) return
        loopStarted = true
        scope.launch {
            while (isActive) {
                val running = controller?.isRunning == true
                updateAudioFocus(running)
                refreshMediaSessionAndNotification()
                delay(500.milliseconds)
            }
        }
    }

    /** 播放时持有 AUDIOFOCUS_GAIN（他方音乐 App 自动暂停）；停止后释放。 */
    private fun updateAudioFocus(running: Boolean) {
        if (running && !holdingAudioFocus) {
            holdingAudioFocus = true
            audioManager.requestAudioFocus(focusRequest!!)
        } else if (!running && holdingAudioFocus) {
            holdingAudioFocus = false
            audioManager.abandonAudioFocusRequest(focusRequest!!)
        }
    }

    /** 永久丢失（他方长期播放）→ 断开；瞬时丢失/闪避不打断流播放。 */
    private fun onAudioFocusChange(change: Int) {
        if (change == AudioManager.AUDIOFOCUS_LOSS) {
            holdingAudioFocus = false
            controller?.disconnect()
        }
    }

    /**
     * 状态变化时同步 MediaSession（播放态 + 元数据）与通知；内容未变化则跳过。
     *
     * MediaSession 供锁屏 / 系统媒体面板 / 蓝牙耳机按键使用，与通知权限无关，必须始终刷新；
     * 仅通知的 notify() 需要 POST_NOTIFICATIONS，故在此单独做权限门禁。
     * canPostNotifications() 已显式 checkSelfPermission；Lint 静态分析无法穿透该 helper，
     * 故用 @SuppressLint 标注这一处已知安全的调用。
     */
    @SuppressLint("MissingPermission")
    private fun refreshMediaSessionAndNotification() {
        val ctrl = controller
        val running = ctrl?.isRunning == true
        val text = notificationText(ctrl, running)
        if (text == lastNotifiedText && running == lastNotifiedRunning) return
        lastNotifiedText = text
        lastNotifiedRunning = running
        updatePlaybackState(running)
        updateMetadata(ctrl, running)
        if (canPostNotifications()) {
            NotificationManagerCompat.from(this).notify(NOTIFICATION_ID, buildNotification())
        }
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

    /** MediaSession 播放态与可用动作（通知按钮/系统媒体面板/蓝牙耳机按键共用）。 */
    private fun updatePlaybackState(running: Boolean) {
        val state = if (running) {
            PlaybackStateCompat.STATE_PLAYING
        } else {
            PlaybackStateCompat.STATE_PAUSED
        }
        // 暴露完整传输动作集：系统据此在锁屏/蓝牙上渲染上下曲与播放暂停键。
        val actions = PlaybackStateCompat.ACTION_PLAY or
            PlaybackStateCompat.ACTION_PAUSE or
            PlaybackStateCompat.ACTION_STOP or
            PlaybackStateCompat.ACTION_SKIP_TO_PREVIOUS or
            PlaybackStateCompat.ACTION_SKIP_TO_NEXT
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

        // 播放/暂停键随运行态切换：播放 = 连接，暂停 = 断开。
        val playPauseIcon = if (running) R.drawable.ic_action_pause else R.drawable.ic_action_play
        val playPauseLabel = if (running) "暂停" else "播放"
        val playPauseAction = if (running) ACTION_DISCONNECT else ACTION_CONNECT

        return NotificationCompat.Builder(this, CHANNEL_ID)
            .setSmallIcon(R.drawable.ic_notification)
            .setContentTitle("Aqua")
            .setContentText(text)
            .setContentIntent(contentPi)
            .setOnlyAlertOnce(true)
            .setOngoing(running)
            .setVisibility(NotificationCompat.VISIBILITY_PUBLIC)
            // MediaStyle：按音乐播放器渲染三键媒体控件（上一曲 / 播放暂停 / 下一曲），
            // 并与 MediaSession 关联，锁屏/媒体面板/蓝牙耳机按键共用同一状态源。
            .setStyle(
                androidx.media.app.NotificationCompat.MediaStyle()
                    .setMediaSession(mediaSession.sessionToken)
                    .setShowActionsInCompactView(0, 1, 2)
            )
            .addAction(R.drawable.ic_skip_previous, "上一曲", servicePendingIntent(ACTION_SKIP_PREVIOUS))
            .addAction(playPauseIcon, playPauseLabel, servicePendingIntent(playPauseAction))
            .addAction(R.drawable.ic_skip_next, "下一曲", servicePendingIntent(ACTION_SKIP_NEXT))
            .build()
    }

    /** 通知栏媒体控件按钮的 PendingIntent：把动作转发回本服务 onStartCommand 处理。 */
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
