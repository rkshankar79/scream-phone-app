#pragma once

#include <cstdint>

namespace screamtest {

/**
 * Plain-old-data snapshot of sender-side congestion-control metrics.
 *
 * This struct is intentionally free of any engine types (no SCReAM headers)
 * so it can live in the interfaces layer and be copied by value across the
 * JNI boundary. Concrete controllers fill the congestion-control fields; the
 * TestSession fills the transport bookkeeping fields.
 */
struct MetricsSnapshot {
    // --- Core congestion-control state ---
    int   cwndBytes = 0;                 // congestion window [bytes]
    int   bytesInFlight = 0;             // estimated bytes in flight [bytes]
    float sRttSeconds = 0.0f;            // smoothed RTT [s]
    float queueDelaySeconds = 0.0f;      // network queue delay, averaged [s]
    float rtpQueueDelaySeconds = 0.0f;   // sender-side RTP queue delay [s]

    // --- Rates (bits per second) ---
    float txBitrateBps = 0.0f;           // transmitted bitrate
    float targetBitrateBps = 0.0f;       // SCReAM recommended target bitrate
    float pacingRateBps = 0.0f;          // pacing rate (approx: cwnd*8 / sRtt)

    // --- Loss / marking ---
    float lossRatePercent = 0.0f;        // short-term loss rate [%]
    bool  lossEpoch = false;             // a loss event occurred recently

    // --- Phase 2 (ECN / L4S) — reserved, populated when CE reception lands ---
    float    ceMarkPercent = 0.0f;
    uint64_t ceMarkCount = 0;

    // --- Transport bookkeeping (filled by TestSession) ---
    uint64_t packetsSent = 0;
    uint64_t bytesSent = 0;
    uint64_t feedbackPackets = 0;
    double   sessionTimeSeconds = 0.0;
};

}  // namespace screamtest
