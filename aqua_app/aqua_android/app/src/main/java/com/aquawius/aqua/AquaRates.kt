package com.aquawius.aqua

import com.aquawius.aqua.RateSampler.Companion.MIN_INTERVAL_MS


/**
 * 计数器的**每秒速率**：纯 Kotlin 派生量，不动 C 侧、不动诊断槽位契约。
 *
 * 诊断快照给的是累计计数（`发送丢弃 128432` 这类值看不出"此刻是否在恶化"），
 * 这里对相邻两次诊断采样做差分、除以真实 elapsed 时间，得到 /s。
 *
 * 为什么放在 App 层而不是 core：速率是**展示派生量**，取决于采样节奏。
 * core 的 diagnostics 契约是"一次聚合快照"（不引入时间状态），CLI 侧另有一套
 * 1s timer 的 counter(total/delta/rate) 机制，两者口径一致（aqua_core/doc/diagnostics.md §1）。
 */

/**
 * 参与速率统计的计数器。用枚举而不是字符串键：UI 查表写错键编译期就报错。
 *
 * @param pick 从诊断快照取累计值；Long 原值（字节类保持字节，速率自然就是 B/s）。
 */
enum class AquaCounter(private val pick: (AquaDiagnostics) -> Long) {
    // ---- 传输（UDP 数据面）----
    AudioPackets({ it.audioFramesAccepted }),
    RxPackets({ it.rxPackets }),
    TxPackets({ it.txPackets }),
    TxBytes({ it.txBytes }),
    RxBytes({ it.rxBytes }),
    RxGapEvents({ it.rxSequenceGapEvents }),
    RxMissingFrames({ it.rxSequenceMissingFrames }),
    RxUnreachable({ it.rxUnreachable }),
    TxDropped({ it.txDropped }),
    TxEnqueueFailures({ it.txEnqueueFailures }),

    // ---- 连接（heartbeat 建连 / 续命）----
    HeartbeatAcks({ it.heartbeatAckCount }),

    // ---- 网络抖动（观测层）----
    StallEvents({ it.jcStallEvents }),
    Reordered({ it.estimatorReorderedPackets }),
    Duplicate({ it.estimatorDuplicatePackets }),
    Late({ it.estimatorLatePackets }),

    // ---- 缓冲（执行层修正）----
    FillEpisodes({ it.jbFillEpisodes }),
    DropEpisodes({ it.jbDropEpisodes }),
    Reanchor({ it.jbReanchorCount }),
    PushRejectedLate({ it.jbPushRejectedLate }),
    PushRejected({ it.jbPushRejected }),
    FillSlots({ it.jbFillCorrectedSlots }),
    DropSlots({ it.jbDropSkippedSlots }),
    SpliceEvents({ it.jbSpliceEvents }),

    // ---- 播放输出（JB 吐出的帧 + 设备 xrun）----
    PullFrames({ it.jbPullFrames }),
    PullSilenceFrames({ it.jbPullSilenceFrames }),
    StreamXrun({ it.streamXrunCount }),

    // ---- 音质（听感损伤）----
    UnderrunEvents({ it.jbUnderrunEvents }),
    ConcealedSlots({ it.jbConcealedSlots }),
    ConcealedSaturated({ it.jbConcealedSaturatedSlots }),
    ;

    internal fun read(d: AquaDiagnostics): Long = pick(d)
}

/** 一次采样得到的速率表：缺项 = 该项本拍无法计算（首拍 / 计数器回退）。 */
class AquaRates internal constructor(private val values: Map<AquaCounter, Double>) {

    /** 速率（单位 /s；字节类为 B/s）；null = 本拍无值。 */
    operator fun get(counter: AquaCounter): Double? = values[counter]
}

/**
 * 速率采样器：持有上一次诊断快照作为差分基准。
 *
 * - 只在**拿到新诊断**时喂入（刷新节流由调用方负责，App 侧是 1s 一次）；
 * - 两次采样间隔低于 [MIN_INTERVAL_MS] 时不重算，直接沿用上次结果——
 *   既避免除出噪声很大的速率，也避免 UI 上速率一闪一闪；
 * - 计数器回退（重连后从 0 重计）时本拍跳过该项，下一拍自然恢复；
 * - [reset] 在建立新连接 / 进入 STOPPED 时调用，防止跨会话差出天文数字。
 */
class RateSampler {
    private var prev: AquaDiagnostics? = null
    private var prevAtMs = 0L
    private var last: AquaRates? = null

    fun reset() {
        prev = null
        prevAtMs = 0L
        last = null
    }

    fun sample(d: AquaDiagnostics, nowMs: Long): AquaRates? {
        val baseline = prev
        val dtMs = nowMs - prevAtMs
        // 基准只在**真的重算**那一拍前移：否则更快的采样节奏会把间隔永远压在阈值之下，
        // 每次都在这里早退 → 永远沿用上次结果，且首拍之后的基准再也立不起来。
        if (baseline == null) {
            prev = d
            prevAtMs = nowMs
            return last
        }
        if (dtMs < MIN_INTERVAL_MS) return last
        prev = d
        prevAtMs = nowMs
        val seconds = dtMs / 1000.0
        val out = HashMap<AquaCounter, Double>(AquaCounter.values().size)
        for (counter in AquaCounter.values()) {
            val before = counter.read(baseline)
            val after = counter.read(d)
            if (after < before) continue // 计数器回退 = 会话重置，本拍跳过
            out[counter] = (after - before) / seconds
        }
        return AquaRates(out).also { last = it }
    }

    companion object {
        /** 差分间隔下限（ms）：低于此值不重算，沿用上次速率。诊断刷新约 1s 一次。 */
        private const val MIN_INTERVAL_MS = 400L
    }
}
