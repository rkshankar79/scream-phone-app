package com.spa.scream

/** Sender session configuration mirrored to the native SessionConfig. */
data class SessionConfig(
    val serverIp: String = "127.0.0.1",
    val serverPort: Int = 30000,
    val localPort: Int = 0,
    val minRateKbps: Int = 1000,
    val startRateKbps: Int = 2000,
    val maxRateKbps: Int = 50000,
    val fps: Int = 50,
    val mtu: Int = 1200,
    /** -1 = Not-ECT, 0 = ECT(0), 1 = ECT(1)/L4S. */
    val ect: Int = -1,
    val delayTargetSeconds: Float = 0.06f,
    val ssrc: Int = 100,
)

/** Immutable metrics snapshot decoded from the native double[] payload. */
data class Metrics(
    val cwndBytes: Int = 0,
    val bytesInFlight: Int = 0,
    val sRttSeconds: Float = 0f,
    val queueDelaySeconds: Float = 0f,
    val rtpQueueDelaySeconds: Float = 0f,
    val txBitrateBps: Float = 0f,
    val targetBitrateBps: Float = 0f,
    val pacingRateBps: Float = 0f,
    val lossRatePercent: Float = 0f,
    val lossEpoch: Boolean = false,
    val ceMarkPercent: Float = 0f,
    val ceMarkCount: Long = 0,
    val packetsSent: Long = 0,
    val bytesSent: Long = 0,
    val feedbackPackets: Long = 0,
    val sessionTimeSeconds: Double = 0.0,
) {
    companion object {
        fun fromArray(a: DoubleArray): Metrics {
            if (a.size < ScreamBridge.METRICS_SIZE) return Metrics()
            return Metrics(
                cwndBytes = a[0].toInt(),
                bytesInFlight = a[1].toInt(),
                sRttSeconds = a[2].toFloat(),
                queueDelaySeconds = a[3].toFloat(),
                rtpQueueDelaySeconds = a[4].toFloat(),
                txBitrateBps = a[5].toFloat(),
                targetBitrateBps = a[6].toFloat(),
                pacingRateBps = a[7].toFloat(),
                lossRatePercent = a[8].toFloat(),
                lossEpoch = a[9] != 0.0,
                ceMarkPercent = a[10].toFloat(),
                ceMarkCount = a[11].toLong(),
                packetsSent = a[12].toLong(),
                bytesSent = a[13].toLong(),
                feedbackPackets = a[14].toLong(),
                sessionTimeSeconds = a[15],
            )
        }
    }
}

/** Receiver-side metrics snapshot decoded from the native double[] payload. */
data class ReceiverMetrics(
    val rtpReceived: Long = 0,
    val feedbackSent: Long = 0,
    val ceMarked: Long = 0,
    val receivedRateBps: Float = 0f,
    val sessionTimeSeconds: Double = 0.0,
) {
    companion object {
        fun fromArray(a: DoubleArray): ReceiverMetrics {
            if (a.size < ScreamBridge.RX_METRICS_SIZE) return ReceiverMetrics()
            return ReceiverMetrics(
                rtpReceived = a[0].toLong(),
                feedbackSent = a[1].toLong(),
                ceMarked = a[2].toLong(),
                receivedRateBps = a[3].toFloat(),
                sessionTimeSeconds = a[4],
            )
        }
    }
}

/**
 * Thin JNI binding to libscream.so. Owns one native TestSession (sender) and
 * one ReceiverSession (receiver) handle. Not thread-safe; access via
 * ScreamEngine which serializes lifecycle calls.
 */
class ScreamBridge {
    private var handle: Long = 0
    private var rxHandle: Long = 0

    fun create() {
        if (handle == 0L) handle = nativeCreate()
    }

    /** Returns null on success, or an error message string on failure. */
    fun start(cfg: SessionConfig): String? {
        if (handle == 0L) create()
        return nativeStart(
            handle,
            cfg.serverIp,
            cfg.serverPort,
            cfg.localPort,
            cfg.minRateKbps,
            cfg.startRateKbps,
            cfg.maxRateKbps,
            cfg.fps,
            cfg.mtu,
            cfg.ect,
            cfg.delayTargetSeconds,
            cfg.ssrc,
        )
    }

    fun stop() {
        if (handle != 0L) nativeStop(handle)
    }

    fun isRunning(): Boolean = handle != 0L && nativeIsRunning(handle)

    fun metrics(): Metrics =
        if (handle == 0L) Metrics() else Metrics.fromArray(nativeGetMetrics(handle))

    fun destroy() {
        if (handle != 0L) {
            nativeDestroy(handle)
            handle = 0
        }
    }

    // --- Receiver role ---

    fun createReceiver() {
        if (rxHandle == 0L) rxHandle = nativeCreateReceiver()
    }

    /** Returns null on success, or an error message string on failure. */
    fun startReceiver(listenPort: Int, ssrc: Int): String? {
        if (rxHandle == 0L) createReceiver()
        return nativeStartReceiver(rxHandle, listenPort, ssrc)
    }

    fun stopReceiver() {
        if (rxHandle != 0L) nativeStopReceiver(rxHandle)
    }

    fun isReceiverRunning(): Boolean = rxHandle != 0L && nativeIsReceiverRunning(rxHandle)

    fun receiverMetrics(): ReceiverMetrics =
        if (rxHandle == 0L) ReceiverMetrics()
        else ReceiverMetrics.fromArray(nativeGetReceiverMetrics(rxHandle))

    private external fun nativeCreate(): Long
    private external fun nativeDestroy(handle: Long)
    private external fun nativeStart(
        handle: Long,
        serverIp: String,
        serverPort: Int,
        localPort: Int,
        minRateKbps: Int,
        startRateKbps: Int,
        maxRateKbps: Int,
        fps: Int,
        mtu: Int,
        ect: Int,
        delayTarget: Float,
        ssrc: Int,
    ): String?

    private external fun nativeStop(handle: Long)
    private external fun nativeIsRunning(handle: Long): Boolean
    private external fun nativeGetMetrics(handle: Long): DoubleArray

    private external fun nativeCreateReceiver(): Long
    private external fun nativeDestroyReceiver(handle: Long)
    private external fun nativeStartReceiver(handle: Long, listenPort: Int, ssrc: Int): String?
    private external fun nativeStopReceiver(handle: Long)
    private external fun nativeIsReceiverRunning(handle: Long): Boolean
    private external fun nativeGetReceiverMetrics(handle: Long): DoubleArray

    companion object {
        const val METRICS_SIZE = 16
        const val RX_METRICS_SIZE = 5

        init {
            System.loadLibrary("scream")
        }
    }
}
