#pragma once

#include <cstdint>

#include "MetricsSnapshot.h"

namespace screamtest {

/**
 * Engine-agnostic contract for a real-time congestion controller.
 *
 * The wrapper/JNI layer depends ONLY on this interface, never on a concrete
 * engine such as SCReAM. This keeps the vendored Ericsson sources in
 * scream_core/ untouched and lets us swap implementations (or inject a mock
 * for unit tests) without changing the transport/session code.
 *
 * Time convention: `timeNtp` is the controller's internal NTP Q16 timestamp
 * (mid 32 bits of an NTP timestamp; 1.0s == 65536). The caller derives it from
 * a monotonic clock. `*Seconds` parameters are plain wall/relative seconds.
 */
class CongestionController {
   public:
    virtual ~CongestionController() = default;

    // --- Lifecycle ---
    // Register the single media/test stream. Bitrates in bits per second.
    virtual void registerStream(uint32_t ssrc,
                                float priority,
                                float minBitrateBps,
                                float startBitrateBps,
                                float maxBitrateBps) = 0;

    virtual void updateBitrate(uint32_t ssrc,
                               float minBitrateBps,
                               float maxBitrateBps) = 0;

    // --- Traffic production ---
    // Notify the controller that `bytesRtp` of media were produced. `isMarker`
    // marks the last packet of a frame.
    virtual void newMediaFrame(uint32_t timeNtp,
                               uint32_t ssrc,
                               int bytesRtp,
                               bool isMarker) = 0;

    // Enqueue an RTP packet pending transmission. The implementation takes a
    // private copy of `data`, so the caller may reuse its buffer immediately.
    virtual bool enqueueRtp(const uint8_t* data,
                            int size,
                            uint32_t ssrc,
                            uint16_t seqNr,
                            bool isMarker,
                            uint32_t rtpTimestamp,
                            float nowSeconds) = 0;

    // --- Pacing / transmission ---
    // Returns 0 -> transmit now, >0 -> seconds until retry, <0 -> nothing to send.
    // On a 0 return, `ssrc` identifies the stream to dequeue from.
    virtual float isOkToTransmit(uint32_t timeNtp, uint32_t& ssrc) = 0;

    // Pop the next pending packet for `ssrc` into `out` (capacity bytes).
    // Returns bytes written, or -1 if none available.
    virtual int dequeueRtp(uint32_t ssrc,
                           uint8_t* out,
                           int capacity,
                           float nowSeconds,
                           uint16_t& seqNr,
                           bool& isMarker,
                           uint32_t& rtpTimestamp,
                           float& rtpQueueDelaySeconds) = 0;

    // Inform the controller a packet was sent. Returns seconds until the next
    // isOkToTransmit() should be attempted.
    virtual float addTransmitted(uint32_t timeNtp,
                                 uint32_t ssrc,
                                 int size,
                                 uint16_t seqNr,
                                 bool isMarker,
                                 float rtpQueueDelaySeconds,
                                 uint32_t rtpTimestamp) = 0;

    // --- Feedback (RTCP, RFC 8888) ---
    virtual void incomingFeedback(uint32_t timeNtp,
                                  const uint8_t* buf,
                                  int size) = 0;

    // --- Rate-control queries ---
    virtual float getTargetBitrate(uint32_t timeNtp, uint32_t ssrc) = 0;
    virtual int getRecommendedMss(uint32_t timeNtp) = 0;
    virtual bool isLossEpoch(uint32_t ssrc) = 0;

    // --- Metrics ---
    virtual MetricsSnapshot getMetrics(uint32_t timeNtp, uint32_t ssrc) = 0;
};

}  // namespace screamtest
