#pragma once

#include <cstdint>

namespace screamtest {

/** Plain-old-data snapshot of receiver-side metrics. */
struct ReceiverMetrics {
    uint64_t rtpReceived = 0;
    uint64_t feedbackSent = 0;
    uint64_t ceMarked = 0;
    float receivedRateBps = 0.0f;
    double sessionTimeSeconds = 0.0;
};

/**
 * Engine-agnostic contract for the SCReAM receiver role: ingest RTP arrivals
 * and emit RFC 8888 congestion-control feedback (RTCP). Mirrors the
 * CongestionController boundary so the wrapper layer never includes scream_core.
 */
class FeedbackReceiver {
   public:
    virtual ~FeedbackReceiver() = default;

    // Record reception of one RTP packet (CE bits from the IP header; 0 until
    // ECN reception is implemented).
    virtual void onRtpPacket(uint32_t timeNtp, uint32_t ssrc, int size,
                             uint16_t seqNr, uint8_t ceBits, bool isMarker,
                             uint32_t rtpTimestamp) = 0;

    // Build pending RFC 8888 feedback into `buf`. Returns the byte count, or
    // <= 0 if no feedback is due (the engine self-gates the cadence).
    virtual int buildFeedback(uint32_t timeNtp, bool isMark, uint8_t* buf,
                              int capacity) = 0;

    virtual ReceiverMetrics getMetrics() = 0;
};

}  // namespace screamtest
