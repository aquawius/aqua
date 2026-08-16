package com.aquawius.aqua

/**
 * 客户端诊断快照，对应 C 侧 aqua_diagnostics_t。
 * 数组顺序与 src/android/jni/aqua_jni.cpp 的 fill_diagnostics_array 一致。
 */
data class AquaDiagnostics(
    // Network
    val rttMs: Double,
    val interarrivalJitterMs: Double,
    val packetsReceived: Double,
    val packetsLost: Double,
    val duplicates: Double,
    val latePackets: Double,
    val recvAudioBytes: Double,
    val recvHelloAcks: Double,
    // JitterBuffer
    val jbCurrentPackets: Double,
    val jbCurrentMs: Double,
    val jbAvgMs: Double,
    val jbMinMs: Double,
    val jbMaxMs: Double,
    val jbCapacityMs: Double,
    // RingBuffer
    val rbCurrentMs: Double,
    val rbAvgMs: Double,
    val rbMinMs: Double,
    val rbMaxMs: Double,
    val rbCapacityMs: Double,
    val underruns: Double,
    val deadlineMisses: Double,
    // Slope
    val shortSlopeSamplesPerS: Double,
    val longSlopeSamplesPerS: Double,
    // End-to-end + drift
    val endToEndMs: Double,
    val driftPpm: Double,
) {
    companion object {
        fun fromArray(a: DoubleArray): AquaDiagnostics? {
            if (a.size != 25) return null
            return AquaDiagnostics(
                rttMs = a[0],
                interarrivalJitterMs = a[1],
                packetsReceived = a[2],
                packetsLost = a[3],
                duplicates = a[4],
                latePackets = a[5],
                recvAudioBytes = a[6],
                recvHelloAcks = a[7],
                jbCurrentPackets = a[8],
                jbCurrentMs = a[9],
                jbAvgMs = a[10],
                jbMinMs = a[11],
                jbMaxMs = a[12],
                jbCapacityMs = a[13],
                rbCurrentMs = a[14],
                rbAvgMs = a[15],
                rbMinMs = a[16],
                rbMaxMs = a[17],
                rbCapacityMs = a[18],
                underruns = a[19],
                deadlineMisses = a[20],
                shortSlopeSamplesPerS = a[21],
                longSlopeSamplesPerS = a[22],
                endToEndMs = a[23],
                driftPpm = a[24],
            )
        }
    }
}
