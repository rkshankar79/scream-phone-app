package com.spa.scream

import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch

/** Number of samples retained per metric for the live charts. */
private const val HISTORY_LEN = 240

/** Upper bound on recorded CSV samples (~7h at 8 Hz); oldest are dropped. */
private const val MAX_SAMPLES = 200_000

/** One recorded metrics row, used for CSV export. */
data class Sample(
    val timestampMs: Long,
    val rttMs: Float,
    val cwndBytes: Int,
    val targetKbps: Float,
    val txKbps: Float,
    val pacingKbps: Float,
    val queueDelayMs: Float,
    val lossPct: Float,
    val cePct: Float,
    val feedbackCount: Long,
    val packetsSent: Long,
)

/** Rolling history of metric samples for charting. */
class MetricsHistory {
    val cwndBytes = ArrayDeque<Float>()
    val sRttMs = ArrayDeque<Float>()
    val txBitrateKbps = ArrayDeque<Float>()
    val targetKbps = ArrayDeque<Float>()
    val queueDelayMs = ArrayDeque<Float>()
    val pacingKbps = ArrayDeque<Float>()
    val lossPercent = ArrayDeque<Float>()

    private fun ArrayDeque<Float>.pushCapped(v: Float) {
        addLast(v)
        while (size > HISTORY_LEN) removeFirst()
    }

    fun add(m: Metrics) {
        cwndBytes.pushCapped(m.cwndBytes.toFloat())
        sRttMs.pushCapped(m.sRttSeconds * 1000f)
        txBitrateKbps.pushCapped(m.txBitrateBps / 1000f)
        targetKbps.pushCapped(m.targetBitrateBps / 1000f)
        queueDelayMs.pushCapped(m.queueDelaySeconds * 1000f)
        pacingKbps.pushCapped(m.pacingRateBps / 1000f)
        lossPercent.pushCapped(m.lossRatePercent)
    }

    fun clear() {
        listOf(cwndBytes, sRttMs, txBitrateKbps, targetKbps, queueDelayMs, pacingKbps, lossPercent)
            .forEach { it.clear() }
    }
}

/**
 * Process-wide owner of the native sender session and the metrics polling loop.
 * The UI observes [metrics]; the foreground service drives start/stop.
 */
object ScreamEngine {
    private val bridge = ScreamBridge().also { it.create() }
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Default)
    private var pollJob: Job? = null

    private val _metrics = MutableStateFlow(Metrics())
    val metrics: StateFlow<Metrics> = _metrics.asStateFlow()

    private val _running = MutableStateFlow(false)
    val running: StateFlow<Boolean> = _running.asStateFlow()

    private val _lastError = MutableStateFlow<String?>(null)
    val lastError: StateFlow<String?> = _lastError.asStateFlow()

    private val _rxMetrics = MutableStateFlow(ReceiverMetrics())
    val rxMetrics: StateFlow<ReceiverMetrics> = _rxMetrics.asStateFlow()

    private val _rxRunning = MutableStateFlow(false)
    val rxRunning: StateFlow<Boolean> = _rxRunning.asStateFlow()

    private var rxPollJob: Job? = null

    val history = MetricsHistory()

    private val samplesLock = Any()
    private val samples = ArrayDeque<Sample>()

    private val _sampleCount = MutableStateFlow(0)
    val sampleCount: StateFlow<Int> = _sampleCount.asStateFlow()

    /** Poll interval for metrics (~8 Hz). */
    var pollIntervalMs: Long = 125

    fun start(cfg: SessionConfig): Boolean {
        if (_running.value) return true
        history.clear()
        clearSamples()
        val err = bridge.start(cfg)
        if (err != null) {
            _lastError.value = err
            return false
        }
        _lastError.value = null
        _running.value = true
        startPolling()
        return true
    }

    fun stop() {
        pollJob?.cancel()
        pollJob = null
        bridge.stop()
        _running.value = false
    }

    fun isSenderRunning(): Boolean = _running.value

    fun isReceiverRunning(): Boolean = _rxRunning.value

    private fun startPolling() {
        pollJob?.cancel()
        pollJob = scope.launch {
            while (isActive) {
                val m = bridge.metrics()
                _metrics.value = m
                history.add(m)
                recordSample(m)
                delay(pollIntervalMs)
            }
        }
    }

    private fun recordSample(m: Metrics) {
        val s = Sample(
            timestampMs = System.currentTimeMillis(),
            rttMs = m.sRttSeconds * 1000f,
            cwndBytes = m.cwndBytes,
            targetKbps = m.targetBitrateBps / 1000f,
            txKbps = m.txBitrateBps / 1000f,
            pacingKbps = m.pacingRateBps / 1000f,
            queueDelayMs = m.queueDelaySeconds * 1000f,
            lossPct = m.lossRatePercent,
            cePct = m.ceMarkPercent,
            feedbackCount = m.feedbackPackets,
            packetsSent = m.packetsSent,
        )
        val size = synchronized(samplesLock) {
            samples.addLast(s)
            while (samples.size > MAX_SAMPLES) samples.removeFirst()
            samples.size
        }
        _sampleCount.value = size
    }

    private fun clearSamples() {
        synchronized(samplesLock) { samples.clear() }
        _sampleCount.value = 0
    }

    /** Serializes the recorded samples to a CSV document (with header row). */
    fun exportCsv(): String {
        val rows = synchronized(samplesLock) { samples.toList() }
        val sb = StringBuilder(64 + rows.size * 80)
        sb.append(
            "timestamp_ms,rtt_ms,cwnd_bytes,target_kbps,tx_kbps,pacing_kbps," +
                "queue_delay_ms,loss_pct,ce_pct,feedback_count,packets_sent\n",
        )
        for (s in rows) {
            sb.append(s.timestampMs).append(',')
                .append(fmt(s.rttMs)).append(',')
                .append(s.cwndBytes).append(',')
                .append(fmt(s.targetKbps)).append(',')
                .append(fmt(s.txKbps)).append(',')
                .append(fmt(s.pacingKbps)).append(',')
                .append(fmt(s.queueDelayMs)).append(',')
                .append(fmt(s.lossPct)).append(',')
                .append(fmt(s.cePct)).append(',')
                .append(s.feedbackCount).append(',')
                .append(s.packetsSent).append('\n')
        }
        return sb.toString()
    }

    private fun fmt(v: Float): String = String.format(java.util.Locale.US, "%.3f", v)

    // --- Receiver role ---

    fun startReceiver(listenPort: Int, ssrc: Int = 100): Boolean {
        if (_rxRunning.value) return true
        val err = bridge.startReceiver(listenPort, ssrc)
        if (err != null) {
            _lastError.value = err
            return false
        }
        _lastError.value = null
        _rxRunning.value = true
        rxPollJob?.cancel()
        rxPollJob = scope.launch {
            while (isActive) {
                _rxMetrics.value = bridge.receiverMetrics()
                delay(pollIntervalMs)
            }
        }
        return true
    }

    fun stopReceiver() {
        rxPollJob?.cancel()
        rxPollJob = null
        bridge.stopReceiver()
        _rxRunning.value = false
    }

    /** Stops sessions (use [ScreamService.stopAll] for service lifecycle), clears metrics/charts/CSV. */
    fun resetForNewTest() {
        stop()
        stopReceiver()
        clearSessionData()
    }

    fun clearSessionData() {
        history.clear()
        clearSamples()
        _metrics.value = Metrics()
        _rxMetrics.value = ReceiverMetrics()
        _lastError.value = null
    }
}
