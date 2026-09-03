package com.aquawius.aqua

import android.annotation.SuppressLint
import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Intent
import android.content.pm.PackageManager
import android.content.pm.ServiceInfo
import android.media.AudioAttributes
import android.media.AudioFocusRequest
import android.media.AudioManager
import android.os.Build
import android.os.IBinder
import android.util.Log
import android.support.v4.media.MediaMetadataCompat
import android.support.v4.media.session.MediaSessionCompat
import android.support.v4.media.session.PlaybackStateCompat
import androidx.core.app.NotificationCompat
import androidx.core.app.NotificationManagerCompat
import androidx.core.app.ServiceCompat
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
 * 前台服务：MediaStyle 媒体通知（播放 / 停止）+ 音频焦点，防止后台播放与自动重连被系统冻结。
 *
 * - MainActivity 在 onCreate 注入 [controller]、启动服务；onDestroy 置空并停止。
 * - MediaSession 承载播放控制（onPlay -> connect，onPause/onStop -> disconnect），
 *   通知与系统媒体面板（锁屏 / 蓝牙耳机按键）共用同一状态源。
 * - 单音频流无曲目队列，不暴露上一曲/下一曲，也不提供播放进度（seekbar）。
 * - 音频焦点：播放期间持有 AUDIOFOCUS_GAIN（其他音乐 App 会自动暂停）；
 *   永久丢失（AUDIOFOCUS_LOSS，他方长期播放）时断开本端，瞬时丢失不打断。
 * - 通知按钮经 PendingIntent → onStartCommand 走主线程调用 controller；系统媒体面板
 *   按键经 MediaSession 回调走主线程，两条路径最终一致。
 */
class AquaService : Service() {

    companion object {
        const val CHANNEL_ID = "aqua_playback"
        const val NOTIFICATION_ID = 1
        const val ACTION_CONNECT = "com.aquawius.aqua.CONNECT"
        const val ACTION_DISCONNECT = "com.aquawius.aqua.DISCONNECT"
        private const val TAG = "AquaService"

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
    // minSdk 26 = O，AudioFocusRequest（API 26+）恒可用，无需版本分支。
    private var holdingAudioFocus = false
    private val audioManager by lazy { getSystemService(AudioManager::class.java) }
    private val focusRequest: AudioFocusRequest by lazy {
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

        // 播放控制：播放 = 连接；暂停/停止 = 断开（单流无曲目，不提供上下曲）。
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
        // 显式声明 mediaPlayback 类型（targetSdk 34+ 强制校验 FGS 类型）。
        ServiceCompat.startForeground(
            this,
            NOTIFICATION_ID,
            buildNotification(),
            ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PLAYBACK,
        )

        // 通知动作按钮入口（Android 12- 及部分 OEM 通知渲染路径）。
        when (intent?.action) {
            ACTION_CONNECT -> controller?.connect()
            ACTION_DISCONNECT -> controller?.disconnect()
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

    /** 播放时持有 AUDIOFOCUS_GAIN（他方音乐 App 自动暂停）；停止后释放。
     *  若开启"允许同时播放"则不申请音频焦点，与其他 App 共存。 */
    private fun updateAudioFocus(running: Boolean) {
        val allowSimultaneous = controller?.allowSimultaneousPlayback == true
        if (allowSimultaneous || !running) {
            // 同时播放模式或已停止：释放可能仍持有的焦点。
            if (holdingAudioFocus) {
                holdingAudioFocus = false
                audioManager.abandonAudioFocusRequest(focusRequest)
            }
            return
        }
        // 独占模式且正在播放：持有焦点。请求可能失败（如来电中），失败时
        // 不持有焦点但播放继续——记录日志便于排障，不因此打断会话。
        if (!holdingAudioFocus) {
            val result = audioManager.requestAudioFocus(focusRequest)
            holdingAudioFocus = result == AudioManager.AUDIOFOCUS_REQUEST_GRANTED
            if (!holdingAudioFocus) {
                Log.w(TAG, "音频焦点请求未获授予（result=$result），继续播放")
            }
        }
    }

    /** 焦点变化：永久丢失 → 断开；瞬时丢失/闪避不打断流播放（继续按 JB 输出）。
     *  注意 AUDIOFOCUS_REQUEST_DELAYED 不视为失败，回调到达时再处理。 */
    private fun onAudioFocusChange(change: Int) {
        when (change) {
            AudioManager.AUDIOFOCUS_LOSS -> {
                // 长期丢失：他方 App 开始播放，独占模式下让位并断开。
                holdingAudioFocus = false
                controller?.disconnect()
            }
            AudioManager.AUDIOFOCUS_LOSS_TRANSIENT,
            AudioManager.AUDIOFOCUS_LOSS_TRANSIENT_CAN_DUCK,
            -> {
                // 瞬时丢失（导航提示、通知音）：焦点请求仍在我们手上，播放继续。
                Log.i(TAG, "音频焦点瞬时丢失（change=$change），保持播放")
            }
            AudioManager.AUDIOFOCUS_GAIN -> {
                holdingAudioFocus = true
            }
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

    /** MediaSession 播放态与可用动作（系统媒体面板/蓝牙耳机按键使用）。
     *  只声明 ACTION_PLAY_PAUSE：系统渲染唯一的中央播放/暂停切换键；
     *  无 SKIP/SEEK_TO 动作 → 不出现上下曲与 seekbar。位置 PLAYBACK_POSITION_UNKNOWN
     *  且元数据不带时长，进一步避免进度条。 */
    private fun updatePlaybackState(running: Boolean) {
        val state = if (running) {
            PlaybackStateCompat.STATE_PLAYING
        } else {
            PlaybackStateCompat.STATE_PAUSED
        }
        val speed = if (running) 1f else 0f
        mediaSession.setPlaybackState(
            PlaybackStateCompat.Builder()
                .setActions(PlaybackStateCompat.ACTION_PLAY_PAUSE)
                .setState(state, PlaybackStateCompat.PLAYBACK_POSITION_UNKNOWN, speed)
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

        // 单一"播放/停止"切换按钮。Android 13+ 媒体面板按 PlaybackState 渲染，
        // 忽略此处 action；Android 12- 及部分 OEM 通知路径按 builder action 渲染，
        // 因此仍需挂一个 action 保证任何路径下都恰好只有一个按钮。
        if (running) {
            builder.addAction(R.drawable.ic_action_pause, "停止", servicePendingIntent(ACTION_DISCONNECT))
        } else {
            builder.addAction(R.drawable.ic_action_play, "播放", servicePendingIntent(ACTION_CONNECT))
        }

        // MediaStyle 音乐通知栏：媒体面板按 PlaybackState 渲染唯一的播放/暂停切换键；
        // 不提供时长与 SKIP 动作，故无上一曲/下一曲与 seekbar。
        builder.setStyle(
            androidx.media.app.NotificationCompat.MediaStyle()
                .setMediaSession(mediaSession.sessionToken)
                .setShowActionsInCompactView(0)
        )
        return builder.build()
    }

    /** 通知动作按钮的 PendingIntent 入口（onStartCommand 分发）。 */
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
