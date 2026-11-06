package com.aquawius.aqua

import androidx.compose.runtime.Immutable

/**
 * 客户端诊断快照，对应 C 侧 aqua_client_diagnostics_t。
 * LongArray（= AQUA_DIAGNOSTICS_FIELD_COUNT，当前 166 项）顺序与
 * aqua_core/src/c_api/android/jni/aqua_jni.cpp 的 nativeGetDiagnostics 写入顺序一致
 * （结构体声明序），两侧同步修改。
 *
 * 存储形态：只保留原始 LongArray，字段以只读 getter 暴露。166 个强类型构造参数
 * 会产生 272 个 DEX 参数寄存器槽（> 255），data class 的 copy$default 还要再加
 * mask/marker，release 下 ART 一验证该类就 VerifyError；数组形态的构造只有 1 个
 * 参数，彻底避开这个上限，也避免以后加字段再次踩线。
 *
 * 音频错误不在快照内（快照 = 组件状态，不承担错误传递）：错误经
 * AquaClient.lastAudioError() + audioErrorEpoch() 独立通道上报。
 *
 * 指标语义见 aqua_core/include/aqua/diagnostics/client_diagnostics_snapshot.h；
 * 各字段为原子近似读值，仅供监控/显示。
 *
 * `@Immutable`：raw 在构造时 copyOf() 且私有，对外只暴露只读 getter。Compose 的
 * 稳定性推导不认识 LongArray（默认 unstable），不显式声明的话，所有接收本类的
 * composable 都会失去跳过重组的能力（data class 时代是自动推导为 stable 的，
 * 改成数组存储后必须自己补上这一条）。
 */
@Immutable
class AquaDiagnostics private constructor(
    private val raw: LongArray,
) {
    val state: AquaRuntimeState get() = AquaRuntimeState.fromCode(raw[0].toInt())
    val playbackRunning: Boolean get() = raw[1] != 0L
    val playbackState: AquaPlaybackState get() = AquaPlaybackState.fromCode(raw[2].toInt())
    val routeMode: AquaRouteMode get() = AquaRouteMode.fromCode(raw[3].toInt())
    val switchOutcome: AquaSwitchOutcome get() = AquaSwitchOutcome.fromCode(raw[4].toInt())
    val switchError: AquaAudioError get() = AquaAudioError.fromCode(raw[5].toInt())
    val switchDurationMs: Int get() = raw[6].toInt()
    val rxPackets: Long get() = raw[7]
    val rxBytes: Long get() = raw[8]
    val rxErrors: Long get() = raw[9]
    val rxUnreachable: Long get() = raw[10]
    val txPackets: Long get() = raw[11]
    val txBytes: Long get() = raw[12]
    val txErrors: Long get() = raw[13]
    val txDropped: Long get() = raw[14]
    val txEnqueueFailures: Long get() = raw[15]
    val txQueueDepth: Long get() = raw[16]
    val heartbeatAckCount: Long get() = raw[17]
    val heartbeatAckMisses: Int get() = raw[18].toInt()
    val heartbeatAckAgeMs: Long get() = raw[19]
    val heartbeatHandshakeSendAttempts: Long get() = raw[20]
    val heartbeatAckMissEvents: Long get() = raw[21]
    val audioFramesAccepted: Long get() = raw[22]
    val rxSequenceGapEvents: Long get() = raw[23]
    val rxSequenceMissingFrames: Long get() = raw[24]
    val malformedDatagrams: Long get() = raw[25]
    val unexpectedSenderDatagrams: Long get() = raw[26]
    val wrongSessionAcks: Long get() = raw[27]
    val audioPayloadMismatches: Long get() = raw[28]
    val nonAudioDatagrams: Long get() = raw[29]
    val heartbeatFailed: Boolean get() = raw[30] != 0L
    val jbWaterLevel: Double get() = Double.fromBits(raw[31])
    val jbUsedSlots: Int get() = raw[32].toInt()
    val jbCapacitySlots: Int get() = raw[33].toInt()
    val jbReanchorCount: Long get() = raw[34]
    val jbReanchorRequests: Long get() = raw[35]
    val jbReanchorCancels: Long get() = raw[36]
    val jbReanchorSanityRejections: Long get() = raw[37]
    val jbLastReanchorSequence: Long get() = raw[38]
    val jbPushAccepted: Long get() = raw[39]
    val jbPushRejected: Long get() = raw[40]
    val jbPushRejectedLate: Long get() = raw[41]
    val jbPushRejectedSlotBusy: Long get() = raw[42]
    val jbPushRejectedInvalid: Long get() = raw[43]
    val jbPushRejectedSanity: Long get() = raw[44]
    val jbPullCalls: Long get() = raw[45]
    val jbPullFrames: Long get() = raw[46]
    val jbPullSilenceFrames: Long get() = raw[47]
    val jbFillEpisodes: Long get() = raw[48]
    val jbFillCorrectedSlots: Long get() = raw[49]
    val jbDropEpisodes: Long get() = raw[50]
    val jbDropSkippedSlots: Long get() = raw[51]
    val jbSpliceEvents: Long get() = raw[52]
    val jbLeadSlots: Int get() = raw[53].toInt()
    val jbPlaySequence: Long get() = raw[54]
    val jbHighestReceivedSequence: Long get() = raw[55]
    val jbConsecutiveSilenceFrames: Long get() = raw[56]
    val jbMaxSilenceRunFrames: Long get() = raw[57]
    val jbEpisodeState: Int get() = raw[58].toInt()
    val jbReanchorPending: Boolean get() = raw[59] != 0L
    val jbReanchorTargetSequence: Long get() = raw[60]
    val playbackPullCalls: Long get() = raw[61]
    val playbackPullFrames: Long get() = raw[62]
    val playbackPullSilenceFrames: Long get() = raw[63]
    val streamBackend: Int get() = raw[64].toInt()
    val streamSampleRate: Long get() = raw[65]
    val streamChannels: Long get() = raw[66]
    val streamPerformanceMode: Int get() = raw[67].toInt()
    val streamFramesPerBurst: Long get() = raw[68]
    val streamBufferCapacityFrames: Long get() = raw[69]
    val streamCallbackCount: Long get() = raw[70]
    val streamCurrentPaddingFrames: Int get() = raw[71].toInt()
    val streamXrunCount: Long get() = raw[72]
    val estimatorJitterMs: Double get() = Double.fromBits(raw[73])
    val estimatorBaseDelayMs: Double get() = Double.fromBits(raw[74])
    val estimatorTransitMs: Double get() = Double.fromBits(raw[75])
    val estimatorReorderedPackets: Long get() = raw[76]
    val estimatorDuplicatePackets: Long get() = raw[77]
    val estimatorLatePackets: Long get() = raw[78]
    val targetSlots: Int get() = raw[79].toInt()
    val targetMs: Double get() = Double.fromBits(raw[80])
    val jbUnderrunEvents: Long get() = raw[81]
    val jbUnderrunFrames: Long get() = raw[82]
    val jbMaxConsecutiveUnderrunSlots: Long get() = raw[83]
    val jbConcealedSlots: Long get() = raw[84]
    val jbConcealedSaturatedSlots: Long get() = raw[85]
    val jbLateUsefulPackets: Long get() = raw[86]
    val jbUnderrunRatio: Double get() = Double.fromBits(raw[87])
    val jbFillDuty: Double get() = Double.fromBits(raw[88])
    val jbDropDuty: Double get() = Double.fromBits(raw[89])
    val jbLeadMs: Double get() = Double.fromBits(raw[90])
    val jcAdaptive: Boolean get() = raw[91] != 0L
    val jcDesiredSlots: Int get() = raw[92].toInt()
    val jcMinSlots: Int get() = raw[93].toInt()
    val jcMaxSlots: Int get() = raw[94].toInt()
    val jcMarginSource: Int get() = raw[95].toInt()
    val jcPath: Int get() = raw[96].toInt()
    val jcFloorBound: Boolean get() = raw[97] != 0L
    val jcCapBound: Boolean get() = raw[98] != 0L
    val jcUnderrunPenalty: Double get() = Double.fromBits(raw[99])
    val jcDwellRemainingMs: Double get() = Double.fromBits(raw[100])
    val jcFallRoomSlots: Double get() = Double.fromBits(raw[101])
    val jcStallEvents: Long get() = raw[102]
    val jcStallPeakMs: Double get() = Double.fromBits(raw[103])
    val jcLastStallGapMs: Double get() = Double.fromBits(raw[104])
    val jcArrivalIntervalMs: Double get() = Double.fromBits(raw[105])
    val jcTailP99Ms: Double get() = Double.fromBits(raw[106])
    val jcTailSamples: Long get() = raw[107]
    val jcTailMarginSlots: Double get() = Double.fromBits(raw[108])
    val jcBandWarningLow: Int get() = raw[109].toInt()
    val jcBandNormalLow: Int get() = raw[110].toInt()
    val jcBandNormalHigh: Int get() = raw[111].toInt()
    val jcBandWarningHigh: Int get() = raw[112].toInt()
    val jcConcealRunSlots: Int get() = raw[113].toInt()
    val jcUnderrunRunSlots: Int get() = raw[114].toInt()
    val switchSeq: Int get() = raw[115].toInt()
    val enginePeriodFrames: Int get() = raw[116].toInt()
    val enginePeriodMinFrames: Int get() = raw[117].toInt()
    val enginePeriodMaxFrames: Int get() = raw[118].toInt()
    val enginePeriodDefaultFrames: Int get() = raw[119].toInt()
    val enginePeriodFundamentalFrames: Int get() = raw[120].toInt()
    val engineResampling: Boolean get() = raw[121] != 0L
    val mixSampleRate: Long get() = raw[122]
    val mixChannels: Long get() = raw[123]
    val bufferSizeFrames: Int get() = raw[124].toInt()
    val outputBufferingFrames: Int get() = raw[125].toInt()
    val endpointLatencyNs: Long get() = raw[126]
    val clockPositionTicks: Long get() = raw[127]
    val clockFrequencyHz: Long get() = raw[128]
    val staleEventsDropped: Long get() = raw[129]
    val emptyEventSkips: Long get() = raw[130]
    val catchupFrames: Int get() = raw[131].toInt()
    val catchupFramesMax: Int get() = raw[132].toInt()
    val callbackFrames: Int get() = raw[133].toInt()
    val sampleRateAdjusted: Boolean get() = raw[134] != 0L
    val callbackCostNs: Long get() = raw[135]
    val callbackCostMaxNs: Long get() = raw[136]
    val callbackOverruns: Long get() = raw[137]
    val rxConsecutiveErrors: Int get() = raw[138].toInt()
    val rxLastErrorCode: Int get() = raw[139].toInt()
    val estimatorPackets: Long get() = raw[140]
    val estimatorGapEvents: Long get() = raw[141]
    val estimatorMissingPackets: Long get() = raw[142]
    val estimatorBaseSet: Boolean get() = raw[143] != 0L
    val udpAddressFromSentinel: Boolean get() = raw[144] != 0L
    val udpFamily: Int get() = raw[145].toInt()
    val oldestReceivedSequence: Long get() = raw[146]
    val concealEnabled: Boolean get() = raw[147] != 0L
    val concealMaxSlots: Int get() = raw[148].toInt()
    val spliceEnabled: Boolean get() = raw[149] != 0L
    val spliceXfadeFrames: Int get() = raw[150].toInt()
    val startupSlots: Int get() = raw[151].toInt()
    val initialTargetSlots: Int get() = raw[152].toInt()
    val reanchorDroppedSlots: Long get() = raw[153]
    val reanchorDroppedLast: Int get() = raw[154].toInt()
    val reanchorReason: Int get() = raw[155].toInt()
    val effectiveMinSlots: Int get() = raw[156].toInt()
    val inStorm: Boolean get() = raw[157] != 0L
    val lastStallMarginSlots: Double get() = Double.fromBits(raw[158])
    val stallPeakCapSlots: Double get() = Double.fromBits(raw[159])
    val fallRateSlotsPerSec: Double get() = Double.fromBits(raw[160])
    val remoteSampleRate: Long get() = raw[161]
    val remoteChannels: Long get() = raw[162]
    val remoteEncoding: Int get() = raw[163].toInt()
    val remoteBitrateKbps: Long get() = raw[164]
    val remoteFrameCount: Long get() = raw[165]

/** 输出静音占比（0..1）：JitterBuffer 吐出的帧中静音的比例（预滚 / 低水位 Hold 的时间轴修正）；无数据时 0。
     *  与「音质」卡的欠载 / 掩盖（没数据可用）不同口径。 */
    val silenceRatio: Double
        get() = if (jbPullFrames > 0) jbPullSilenceFrames.toDouble() / jbPullFrames else 0.0

    /** margin 胜出方（jcMarginSource 的展示名）。 */
    val marginSourceLabel: String
        get() = when (jcMarginSource) {
            1 -> "断流峰值"
            else -> "尾部分位"
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
     * 顶上限 > 贴下限 > 期望与现值不等（被限速/锁跌/风暴冻结按住）> 稳态。
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
        /**
         * 扁平诊断数组的字段数。**必须与 native 侧的
         * AQUA_DIAGNOSTICS_FIELD_COUNT 一致**（C 头 / aqua_jni.cpp）。
         *
         * C++ 侧的结构体、capi 桥接、JNI 写入序、字段总数是**同一张清单派生**的
         * （见 aqua_capi.h 的 AQUA_DIAGNOSTICS_TAIL_FIELDS），编译期不会漂移；
         * 但 Kotlin 这边只能手写常量——对不上时下面的守卫会**整份丢弃并打日志**
         * （而不是错位解码出一堆看似合理的假数字）。
         */
        const val EXPECTED_FIELD_COUNT = 166

        fun fromArray(a: LongArray): AquaDiagnostics? {
            if (a.size != EXPECTED_FIELD_COUNT) {
                // 响亮失败：字段数与 native 侧对不上（改了 core 的诊断字段却忘了
                // 同步这里），**整份丢弃**而不是错位解码。必须打日志——否则
                // 症状是"诊断面板停在最后一次的数值上"，看不出是哪儿对不上。
                android.util.Log.e(
                    "AquaDiagnostics",
                    "diagnostics field count mismatch: expected $EXPECTED_FIELD_COUNT, got ${a.size}; " +
                        "the native diagnostics contract changed but this decoder was not updated",
                )
                return null
            }
            // 复制一份：调用方（JNI 桥）可能复用/覆盖传入的数组。
            return AquaDiagnostics(a.copyOf())
        }
    }

    // 数组内容相等 = 同一份快照；与原先 data class 的逐字段比较语义一致。
    override fun equals(other: Any?): Boolean {
        if (this === other) return true
        if (other !is AquaDiagnostics) return false
        return raw.contentEquals(other.raw)
    }

    override fun hashCode(): Int = raw.contentHashCode()
}
