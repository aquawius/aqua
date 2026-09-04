package com.aquawius.aqua

/**
 * 客户端诊断快照，对应 C 侧 aqua_client_diagnostics_t。
 * LongArray(66) 顺序与 aqua_core/src/c_api/android/jni/aqua_jni.cpp 的
 * nativeGetDiagnostics 写入顺序一致（结构体声明序），两侧同步修改。
 *
 * 音频错误不在快照内（快照 = 组件状态，不承担错误传递）：错误经
 * AquaClient.lastAudioError() + audioErrorEpoch() 独立通道上报。
 *
 * 指标语义见 aqua_core/include/aqua/diagnostics/client_diagnostics_snapshot.h；
 * 各字段为原子近似读值，仅供监控/显示。
 */
data class AquaDiagnostics(
    // ---- 生命周期 ----
    val state: AquaRuntimeState,
    val playbackRunning: Boolean,
    val playbackState: AquaPlaybackState,
    // ---- 播放路由与切换事务（playback_switching_design.md §9）----
    val routeMode: AquaRouteMode,
    val switchOutcome: AquaSwitchOutcome,
    val switchError: AquaAudioError, // 切换链上最后失败原因
    val switchDurationMs: Int, // 最近一次切换事务耗时（ms）
    // ---- net ----
    val rxPackets: Long,
    val rxBytes: Long,
    val rxErrors: Long,
    val txPackets: Long,
    val txBytes: Long,
    val txErrors: Long,
    val txDropped: Long,
    val txEnqueueFailures: Long,
    val txQueueDepth: Long,
    val helloAckCount: Long,
    val helloAckMisses: Int,
    val helloAckAgeMs: Long, // <0 = 尚未收到 ACK
    val helloSendAttempts: Long,
    val helloAckMissEvents: Long,
    val audioFramesAccepted: Long,
    val malformedDatagrams: Long,
    val unexpectedSenderDatagrams: Long,
    val wrongSessionAcks: Long,
    val audioPayloadMismatches: Long,
    val nonAudioDatagrams: Long,
    val helloFailed: Boolean,
    // ---- jitter buffer ----
    val jbWaterLevel: Double, // lead_slots / capacity
    val jbUsedSlots: Int,
    val jbCapacitySlots: Int,
    val jbReanchorCount: Long,
    val jbReanchorRequests: Long,
    val jbReanchorCancels: Long,
    val jbReanchorSanityRejections: Long,
    val jbLastReanchorSequence: Long,
    val jbPushAccepted: Long,
    val jbPushRejected: Long,
    val jbPushRejectedLate: Long,
    val jbPushRejectedSlotBusy: Long,
    val jbPushRejectedInvalid: Long,
    val jbPushRejectedSanity: Long,
    val jbPullCalls: Long,
    val jbPullFrames: Long,
    val jbPullSilenceFrames: Long,
    val jbFillEpisodes: Long,
    val jbFillCorrectedSlots: Long,
    val jbDropEpisodes: Long,
    val jbDropSkippedSlots: Long,
    // ---- jitter buffer gauge（当前态，与累计 counter 互补）----
    val jbLeadSlots: Int, // lead = highest - play + 1（绝对值）
    val jbPlaySequence: Long, // 播放头序列（未锚定 = 0）
    val jbHighestReceivedSequence: Long, // 已收到的最高序列
    val jbConsecutiveSilenceFrames: Long, // 当前连续静音 run
    val jbMaxSilenceRunFrames: Long, // 本次运行最长静音 run
    val jbEpisodeState: Int, // 0=None 1=Filling 2=Dropping
    val jbReanchorPending: Boolean, // 有待应用的 reanchor 请求
    val jbReanchorTargetSequence: Long, // 待应用目标序列（无 = 0）
    // ---- playback 消费侧 ----
    val playbackPullCalls: Long,
    val playbackPullFrames: Long,
    val playbackPullSilenceFrames: Long,
    // ---- playback 输出流实际运行参数（后端 open 后回读）----
    val streamBackend: Int, // 0=none 1=AAudio 2=WASAPI
    val streamSampleRate: Long,
    val streamChannels: Long,
    val streamPerformanceMode: Int, // 10=none 11=power_saving 12=low_latency
    val streamFramesPerBurst: Long,
    val streamBufferCapacityFrames: Long,
) {
    /** 静音帧占比（0..1）：pull 出的帧中静音的比例；无数据时 0。 */
    val silenceRatio: Double
        get() = if (jbPullFrames > 0) jbPullSilenceFrames.toDouble() / jbPullFrames else 0.0

    companion object {
        fun fromArray(a: LongArray): AquaDiagnostics? {
            if (a.size != 66) return null
            var i = 0
            fun u(): Long = a[i++]
            fun d(): Double {
                // double 以位模式传过 JNI（见 aqua_jni.cpp writeF64）。
                return Double.fromBits(a[i++])
            }
            fun b(): Boolean = a[i++] != 0L
            return AquaDiagnostics(
                state = AquaRuntimeState.fromCode(a[i].toInt()).also { i++ },
                playbackRunning = b(),
                playbackState = AquaPlaybackState.fromCode(a[i].toInt()).also { i++ },
                routeMode = AquaRouteMode.fromCode(a[i].toInt()).also { i++ },
                switchOutcome = AquaSwitchOutcome.fromCode(a[i].toInt()).also { i++ },
                switchError = AquaAudioError.fromCode(a[i].toInt()).also { i++ },
                switchDurationMs = a[i].toInt().also { i++ },
                rxPackets = u(), rxBytes = u(), rxErrors = u(),
                txPackets = u(), txBytes = u(), txErrors = u(),
                txDropped = u(), txEnqueueFailures = u(), txQueueDepth = u(),
                helloAckCount = u(),
                helloAckMisses = a[i].toInt().also { i++ },
                helloAckAgeMs = u(),
                helloSendAttempts = u(), helloAckMissEvents = u(),
                audioFramesAccepted = u(), malformedDatagrams = u(),
                unexpectedSenderDatagrams = u(), wrongSessionAcks = u(),
                audioPayloadMismatches = u(), nonAudioDatagrams = u(),
                helloFailed = b(),
                jbWaterLevel = d(),
                jbUsedSlots = a[i].toInt().also { i++ },
                jbCapacitySlots = a[i].toInt().also { i++ },
                jbReanchorCount = u(), jbReanchorRequests = u(), jbReanchorCancels = u(),
                jbReanchorSanityRejections = u(), jbLastReanchorSequence = u(),
                jbPushAccepted = u(), jbPushRejected = u(), jbPushRejectedLate = u(),
                jbPushRejectedSlotBusy = u(), jbPushRejectedInvalid = u(),
                jbPushRejectedSanity = u(),
                jbPullCalls = u(), jbPullFrames = u(), jbPullSilenceFrames = u(),
                jbFillEpisodes = u(), jbFillCorrectedSlots = u(),
                jbDropEpisodes = u(), jbDropSkippedSlots = u(),
                jbLeadSlots = a[i].toInt().also { i++ },
                jbPlaySequence = u(),
                jbHighestReceivedSequence = u(),
                jbConsecutiveSilenceFrames = u(),
                jbMaxSilenceRunFrames = u(),
                jbEpisodeState = a[i].toInt().also { i++ },
                jbReanchorPending = b(),
                jbReanchorTargetSequence = u(),
                playbackPullCalls = u(), playbackPullFrames = u(),
                playbackPullSilenceFrames = u(),
                streamBackend = a[i].toInt().also { i++ },
                streamSampleRate = u(),
                streamChannels = u(),
                streamPerformanceMode = a[i].toInt().also { i++ },
                streamFramesPerBurst = u(),
                streamBufferCapacityFrames = u(),
            )
        }
    }
}
