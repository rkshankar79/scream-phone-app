#pragma once

#include <cstdint>

#include "../interfaces/CongestionController.h"

// Forward declarations of the vendored SCReAM types (global namespace).
// Including the actual headers is confined to ScreamController.cpp so that the
// wrapper layer never pulls scream_core/ into its translation units.
class ScreamV2Tx;
class RtpQueue;

namespace screamtest {

/** Construction-time configuration mapped onto the ScreamV2Tx constructor. */
struct ScreamConfig {
    float lossBeta = 0.8f;                 // CWND scale on loss
    float ecnCeBeta = 0.9f;                // CWND scale on classic ECN-CE
    float queueDelayTargetSeconds = 0.06f; // delay target
    int initRateKbps = 2000;
    bool l4s = false;                      // true => ECT(1)/L4S scalable response
    bool enableClockDriftCompensation = false;
    float hysteresis = 0.05f;
    float maxRtpQueueDelaySeconds = 0.2f;
};

/**
 * Concrete CongestionController backed by Ericsson's ScreamV2Tx.
 *
 * This adapter owns one ScreamV2Tx instance and one RtpQueue, and translates
 * the engine-agnostic interface calls into the SCReAM public API. It is NOT
 * internally synchronized; the owning TestSession serializes all calls under a
 * single mutex (mirroring the reference wrapper's lock_scream discipline).
 */
class ScreamController : public CongestionController {
   public:
    explicit ScreamController(const ScreamConfig& cfg);
    ~ScreamController() override;

    void registerStream(uint32_t ssrc, float priority, float minBitrateBps,
                        float startBitrateBps, float maxBitrateBps) override;
    void updateBitrate(uint32_t ssrc, float minBitrateBps,
                       float maxBitrateBps) override;
    void newMediaFrame(uint32_t timeNtp, uint32_t ssrc, int bytesRtp,
                       bool isMarker) override;
    bool enqueueRtp(const uint8_t* data, int size, uint32_t ssrc, uint16_t seqNr,
                    bool isMarker, uint32_t rtpTimestamp, float nowSeconds) override;
    float isOkToTransmit(uint32_t timeNtp, uint32_t& ssrc) override;
    int dequeueRtp(uint32_t ssrc, uint8_t* out, int capacity, float nowSeconds,
                   uint16_t& seqNr, bool& isMarker, uint32_t& rtpTimestamp,
                   float& rtpQueueDelaySeconds) override;
    float addTransmitted(uint32_t timeNtp, uint32_t ssrc, int size, uint16_t seqNr,
                         bool isMarker, float rtpQueueDelaySeconds,
                         uint32_t rtpTimestamp) override;
    void incomingFeedback(uint32_t timeNtp, const uint8_t* buf, int size) override;
    float getTargetBitrate(uint32_t timeNtp, uint32_t ssrc) override;
    int getRecommendedMss(uint32_t timeNtp) override;
    bool isLossEpoch(uint32_t ssrc) override;
    MetricsSnapshot getMetrics(uint32_t timeNtp, uint32_t ssrc) override;

   private:
    ScreamV2Tx* tx_ = nullptr;
    RtpQueue* rtpQueue_ = nullptr;
    uint32_t ssrc_ = 0;
    float lastRtpQueueDelaySeconds_ = 0.0f;
    float lastTargetBitrateBps_ = 0.0f;
};

}  // namespace screamtest
