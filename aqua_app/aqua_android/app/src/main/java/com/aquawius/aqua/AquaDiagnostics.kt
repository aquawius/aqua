package com.aquawius.aqua

/**
 * 客户端诊断快照，对应 C 侧 aqua_client_diagnostics_t。
 * LongArray(111)（= AQUA_DIAGNOSTICS_FIELD_COUNT）顺序与 aqua_core/src/c_api/android/jni/aqua_jni.cpp 的
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
    val heartbeatAckCount: Long,
    val heartbeatAckMisses: Int,
    val heartbeatAckAgeMs: Long, // <0 = 尚未收到 ACK
    val heartbeatHandshakeSendAttempts: Long,
    val heartbeatAckMissEvents: Long,
    val audioFramesAccepted: Long,
    val rxSequenceGapEvents: Long, // 音频接收序列缺口事件数（"收到流缺口"≠"丢包"）
    val rxSequenceMissingFrames: Long, // 缺口累计缺失帧数
    val malformedDatagrams: Long,
    val unexpectedSenderDatagrams: Long,
    val wrongSessionAcks: Long,
    val audioPayloadMismatches: Long,
    val nonAudioDatagrams: Long,
    val heartbeatFailed: Boolean,
    // ---- JB ----
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
    // ---- JB gauge（当前态，与累计 counter 互补）----
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
    // ---- stream 运行期统计（Gauge）----
    val streamCallbackCount: Long, // 后端实际回调次数（WASAPI 渲染趟 / AAudio data callback）
    val streamCurrentPaddingFrames: Int, // 端点缓冲当前填充（WASAPI；AAudio 未知 = 0）
    val streamXrunCount: Long, // 欠载/超限（AAudio xRun；WASAPI = 0）
    // ---- Phase 0 网络观测（0.3.0 末尾追加）----
    val estimatorJitterMs: Double, // RFC 3550 interarrival jitter（观测量 ≠ target）
    val estimatorBaseDelayMs: Double, // 路径底噪
    val estimatorTransitMs: Double, // 当前相对 transit
    val estimatorReorderedPackets: Long, // 乱序到达
    val estimatorDuplicatePackets: Long, // 重复到达
    val estimatorLatePackets: Long, // 落后观测窗之外
    // ---- Phase 1 自适应 target（末尾追加）----
    val targetSlots: Int, // 当前 target（固定模式 = 构造值；自适应 = controller 输出）
    val targetMs: Double, // target 换算毫秒
    // ---- Phase 2 欠载预算 + PCM concealment（末尾追加）----
    // underrun = 播放头推进到没有真实 PCM 可用的 slot（conceal 掩盖帧也算），
    // 不含 pre-roll / 低水位 Hold 的静音（那是时间轴修正）。
    val jbUnderrunEvents: Long, // 缺帧 run 次数
    val jbUnderrunFrames: Long, // 掩盖帧 + 缺帧静音帧
    val jbMaxConsecutiveUnderrunSlots: Long, // 单次最长缺帧 slot 数
    val jbConcealedSlots: Long, // repeat-last 掩盖的 slot 数
    val jbConcealedSaturatedSlots: Long, // 超连续上限退回静音的 slot 数
    val jbLateUsefulPackets: Long, // 迟到但落在 conceal 窗口内（本可用）的包数
    val jbUnderrunRatio: Double, // underrun_frames / pull_frames
    val jbFillDuty: Double, // Fill 慢放多播帧占比
    val jbDropDuty: Double, // Drop 跳过 slot 帧占比
    // ---- lead 毫秒（细则 §11：lead 与 target/jitter 同快照；末尾追加）----
    val jbLeadMs: Double, // 实际 lead 换算毫秒（与 targetMs 同口径）
    // ---- Buffer 决策层观测（jitter_control 组，末尾追加）----
    // 上面那批是"结果与累计计数"，这一批是"决策与阈值"：回答为什么 target /
    // 水位 / 掩盖是现在这样。仅自适应模式下有值（jcAdaptive=false 时全 0）。
    // 决策层 11 项
    val jcAdaptive: Boolean, // target 是否由 TargetController 输出（false = 固定模式 --jb-fixed-target）
    val jcDesiredSlots: Int, // 未限速期望值；与 targetSlots 不等 = 被限速 / dwell / 死区按住
    val jcMinSlots: Int, // 生效下限 = max(--jb-min-target, 几何地板 + 1)
    val jcMaxSlots: Int, // 结构上限 = 2/3 × capacity，不可顶穿
    val jcMarginSource: Int, // margin 胜出方：0=kJ 1=stall_peak
    val jcPath: Int, // 本拍收敛路径：0=steady 1=rise 2=fall 3=dwell_lock 4=deadband 5=no_time_base
    val jcFloorBound: Boolean, // desired 被下限抬起（margin 失算，安全网托住）
    val jcCapBound: Boolean, // desired 被结构上限夹住（正在兜底）
    val jcUnderrunPenalty: Double, // 欠载反馈抬升量（槽）；>0 = 闭环在工作（预期，非故障）
    val jcDwellRemainingMs: Double, // 涨后锁跌剩余（ms）；>0 = 正在锁跌（峰值保持）
    val jcFallRoomSlots: Double, // 本拍跌侧限速额度（槽）；解释"这一拍为什么只降一格"
    // 观测层尾部 4 项
    val jcStallEvents: Long, // 被 stall 门剔除的断流次数（不进 J）
    val jcStallPeakMs: Double, // 近期最坏到达间隙的衰减最大值
    val jcLastStallGapMs: Double, // 最近一次 stall 的到达间隔
    val jcArrivalIntervalMs: Double, // 最近一个按序包的到达间隔
    // 执行层 6 项：水位带同快照一组 + 当前 run
    val jcBandWarningLow: Int,
    val jcBandNormalLow: Int,
    val jcBandNormalHigh: Int,
    val jcBandWarningHigh: Int,
    val jcConcealRunSlots: Int, // 当前连续掩盖槽数（0 = 未在掩盖）
    val jcUnderrunRunSlots: Int, // 当前连续缺帧槽数（0 = 正常）
    // ---- 播放设备切换事务序号（末尾追加）----
    // 每笔切换事务递增一次；outcome+error 会重复（见 AquaController 的提示判据）。
    val switchSeq: Int,
) {
    /** 输出静音占比（0..1）：JitterBuffer 吐出的帧中静音的比例（预滚 / 低水位 Hold 的时间轴修正）；无数据时 0。
     *  与「音质」卡的欠载 / 掩盖（没数据可用）不同口径。 */
    val silenceRatio: Double
        get() = if (jbPullFrames > 0) jbPullSilenceFrames.toDouble() / jbPullFrames else 0.0

    /** margin 胜出方（jcMarginSource 的展示名）。 */
    val marginSourceLabel: String
        get() = when (jcMarginSource) {
            1 -> "断流峰值"
            2 -> "尾部分位"
            else -> "抖动均值"
        }

    /** 本拍收敛路径（jcPath 的展示名）。 */
    val pathLabel: String
        get() = when (jcPath) {
            1 -> "上调"
            2 -> "回落中"
            3 -> "锁跌保持"
            4 -> "死区滞留"
            5 -> "无时间基"
            6 -> "风暴保持"
            else -> "稳态"
        }

    /** 当前时间轴修正动作（jbEpisodeState 的展示名）：None/Filling/Dropping。 */
    val episodeStateLabel: String
        get() = when (jbEpisodeState) {
            1 -> "Fill 中"
            2 -> "Drop 中"
            else -> "稳态"
        }

    /**
     * target 当前处境的一句话结论（按诊断优先级从高到低）：
     * 顶上限 > 贴下限 > 期望与现值不等（被限速/锁跌/死区按住）> 稳态。
     * 这是把 jcDesiredSlots / jcMaxSlots / jcMinSlots / jcPath 四个量合成一句人话。
     */
    val targetStateLabel: String
        get() = when {
            !jcAdaptive -> "固定模式"
            jcCapBound -> "顶到上限"
            jcFloorBound -> "贴在下限"
            jcDesiredSlots > targetSlots -> "已被抬起"
            jcDesiredSlots < targetSlots -> pathLabel
            else -> "稳态"
        }

    /** 是否正在掩盖（当前连续掩盖 > 0）。 */
    val concealing: Boolean get() = jcConcealRunSlots > 0

    /** 是否正在缺帧（当前连续缺帧 > 0）。 */
    val underrunning: Boolean get() = jcUnderrunRunSlots > 0

    companion object {
        fun fromArray(a: LongArray): AquaDiagnostics? {
            if (a.size != 111) return null
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
                heartbeatAckCount = u(),
                heartbeatAckMisses = a[i].toInt().also { i++ },
                heartbeatAckAgeMs = u(),
                heartbeatHandshakeSendAttempts = u(), heartbeatAckMissEvents = u(),
                audioFramesAccepted = u(),
                rxSequenceGapEvents = u(), rxSequenceMissingFrames = u(),
                malformedDatagrams = u(),
                unexpectedSenderDatagrams = u(), wrongSessionAcks = u(),
                audioPayloadMismatches = u(), nonAudioDatagrams = u(),
                heartbeatFailed = b(),
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
                streamCallbackCount = u(),
                streamCurrentPaddingFrames = a[i].toInt().also { i++ },
                streamXrunCount = u(),
                estimatorJitterMs = d(),
                estimatorBaseDelayMs = d(),
                estimatorTransitMs = d(),
                estimatorReorderedPackets = u(),
                estimatorDuplicatePackets = u(),
                estimatorLatePackets = u(),
                targetSlots = a[i].toInt().also { i++ },
                targetMs = d(),
                jbUnderrunEvents = u(), jbUnderrunFrames = u(),
                jbMaxConsecutiveUnderrunSlots = u(),
                jbConcealedSlots = u(), jbConcealedSaturatedSlots = u(),
                jbLateUsefulPackets = u(),
                jbUnderrunRatio = d(), jbFillDuty = d(), jbDropDuty = d(),
                jbLeadMs = d(),
                // jitter_control 组（顺序与 aqua_jitter_control_stats_t 一致）
                jcAdaptive = b(),
                jcDesiredSlots = a[i].toInt().also { i++ },
                jcMinSlots = a[i].toInt().also { i++ },
                jcMaxSlots = a[i].toInt().also { i++ },
                jcMarginSource = a[i].toInt().also { i++ },
                jcPath = a[i].toInt().also { i++ },
                jcFloorBound = b(),
                jcCapBound = b(),
                jcUnderrunPenalty = d(),
                jcDwellRemainingMs = d(),
                jcFallRoomSlots = d(),
                jcStallEvents = u(),
                jcStallPeakMs = d(),
                jcLastStallGapMs = d(),
                jcArrivalIntervalMs = d(),
                jcBandWarningLow = a[i].toInt().also { i++ },
                jcBandNormalLow = a[i].toInt().also { i++ },
                jcBandNormalHigh = a[i].toInt().also { i++ },
                jcBandWarningHigh = a[i].toInt().also { i++ },
                jcConcealRunSlots = a[i].toInt().also { i++ },
                jcUnderrunRunSlots = a[i].toInt().also { i++ },
                switchSeq = a[i].toInt().also { i++ },
            )
        }
    }
}
