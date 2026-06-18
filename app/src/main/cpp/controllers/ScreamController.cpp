#include "ScreamController.h"

#include <cstdlib>
#include <cstring>

// The only place allowed to include the vendored SCReAM sources.
#include "../scream_core/RtpQueue.h"
#include "../scream_core/ScreamTx.h"

// SCReAM's RtpQueue::clear() (invoked when the RTP queue is discarded because
// its delay exceeded the limit) releases every packet it still holds through
// this global hook. We allocate those buffers in ScreamController::enqueueRtp,
// so we free them here. The signature/linkage must match the
// `extern void packet_free(void*, uint32_t)` declaration in RtpQueue.cpp.
void packet_free(void* buf, uint32_t /*ssrc*/) { std::free(buf); }

namespace screamtest {

ScreamController::ScreamController(const ScreamConfig& cfg) {
    // Let SCReAM pick a sensible initial cwnd (0 => default) and use library
    // default headroom/pacing constants; only expose the knobs we care about.
    tx_ = new ScreamV2Tx(
        /* lossBeta */ cfg.lossBeta,
        /* ecnCeBeta */ cfg.ecnCeBeta,
        /* queueDelayTargetMin */ cfg.queueDelayTargetSeconds,
        /* cwnd */ 0,
        /* packetPacingHeadroom */ kPacketPacingHeadRoom,
        /* maxAdaptivePacingRateScale */ kMaxAdaptivePacingRateScale,
        /* bytesInFlightHeadRoom */ kBytesInFlightHeadRoom,
        /* multiplicativeIncreaseScalefactor */ kMultiplicativeIncreaseScalefactor,
        /* isL4s */ cfg.l4s,
        /* maxWindowHeadroom */ 3.0f,
        /* enableSbd */ kEnableSbd,
        /* enableClockDriftCompensation */ cfg.enableClockDriftCompensation);
}

ScreamController::~ScreamController() {
    delete tx_;
    delete rtpQueue_;
}

void ScreamController::registerStream(uint32_t ssrc, float priority,
                                      float minBitrateBps, float startBitrateBps,
                                      float maxBitrateBps) {
    ssrc_ = ssrc;
    rtpQueue_ = new RtpQueue();
    tx_->registerNewStream(rtpQueue_, ssrc, priority, minBitrateBps,
                           startBitrateBps, maxBitrateBps,
                           /* maxRtpQueueDelay */ 0.2f,
                           /* isAdaptiveTargetRateScale */ false,
                           /* hysteresis */ 0.05f);
}

void ScreamController::updateBitrate(uint32_t ssrc, float minBitrateBps,
                                     float maxBitrateBps) {
    tx_->updateBitrateStream(ssrc, minBitrateBps, maxBitrateBps);
}

void ScreamController::newMediaFrame(uint32_t timeNtp, uint32_t ssrc,
                                     int bytesRtp, bool isMarker) {
    tx_->newMediaFrame(timeNtp, ssrc, bytesRtp, isMarker);
}

bool ScreamController::enqueueRtp(const uint8_t* data, int size, uint32_t ssrc,
                                  uint16_t seqNr, bool isMarker,
                                  uint32_t rtpTimestamp, float nowSeconds) {
    // SCReAM's RtpQueue stores the packet pointer without copying, so we own a
    // heap copy here and release it on dequeue.
    void* buf = std::malloc(static_cast<size_t>(size));
    if (buf == nullptr) {
        return false;
    }
    std::memcpy(buf, data, static_cast<size_t>(size));
    bool ok = rtpQueue_->push(buf, size, ssrc, seqNr, isMarker, nowSeconds,
                              rtpTimestamp);
    if (!ok) {
        std::free(buf);
        return false;
    }
    return true;
}

float ScreamController::isOkToTransmit(uint32_t timeNtp, uint32_t& ssrc) {
    return tx_->isOkToTransmit(timeNtp, ssrc);
}

int ScreamController::dequeueRtp(uint32_t ssrc, uint8_t* out, int capacity,
                                 float nowSeconds, uint16_t& seqNr,
                                 bool& isMarker, uint32_t& rtpTimestamp,
                                 float& rtpQueueDelaySeconds) {
    rtpQueueDelaySeconds = rtpQueue_->getDelay(nowSeconds);

    void* buf = nullptr;
    int size = 0;
    uint32_t outSsrc = 0;
    unsigned short sq = 0;
    bool mk = false;
    uint32_t ts = 0;
    if (!rtpQueue_->pop(&buf, size, outSsrc, sq, mk, ts)) {
        return -1;
    }
    if (size > capacity) {
        size = capacity;
    }
    if (buf != nullptr) {
        std::memcpy(out, buf, static_cast<size_t>(size));
        std::free(buf);
    }
    seqNr = sq;
    isMarker = mk;
    rtpTimestamp = ts;
    lastRtpQueueDelaySeconds_ = rtpQueueDelaySeconds;
    return size;
}

float ScreamController::addTransmitted(uint32_t timeNtp, uint32_t ssrc, int size,
                                       uint16_t seqNr, bool isMarker,
                                       float rtpQueueDelaySeconds,
                                       uint32_t rtpTimestamp) {
    return tx_->addTransmitted(timeNtp, ssrc, size, seqNr, isMarker,
                               rtpQueueDelaySeconds, rtpTimestamp);
}

void ScreamController::incomingFeedback(uint32_t timeNtp, const uint8_t* buf,
                                        int size) {
    // The SCReAM API takes a non-const buffer but does not retain it.
    tx_->incomingStandardizedFeedback(
        timeNtp, const_cast<unsigned char*>(buf), size);
}

float ScreamController::getTargetBitrate(uint32_t timeNtp, uint32_t ssrc) {
    float r = tx_->getTargetBitrate(timeNtp, ssrc);
    if (r > 0.0f) {
        lastTargetBitrateBps_ = r;
    }
    return r;
}

int ScreamController::getRecommendedMss(uint32_t timeNtp) {
    return tx_->getRecommendedMss(timeNtp);
}

bool ScreamController::isLossEpoch(uint32_t ssrc) {
    return tx_->isLossEpoch(ssrc);
}

MetricsSnapshot ScreamController::getMetrics(uint32_t timeNtp, uint32_t ssrc) {
    (void)timeNtp;
    MetricsSnapshot m;
    m.cwndBytes = tx_->getCwnd();
    m.sRttSeconds = tx_->getSRtt();
    m.txBitrateBps = tx_->getStatisticsItem(AVG_RATE);
    m.queueDelaySeconds = tx_->getStatisticsItem(AVG_QUEUE_DELAY);
    m.rtpQueueDelaySeconds = lastRtpQueueDelaySeconds_;
    m.lossRatePercent = tx_->getStatisticsItem(LOSS_RATE);
    m.ceMarkPercent = tx_->getStatisticsItem(CE_RATE);
    // targetBitrate is driven from the traffic-generation path to avoid
    // perturbing SCReAM's hysteresis/loss state from the metrics reader.
    m.targetBitrateBps = lastTargetBitrateBps_;
    // No public bytes-in-flight getter on the untouched core; pacing rate is
    // approximated from the self-clock relation cwnd*8 / sRtt.
    m.pacingRateBps =
        (m.sRttSeconds > 0.0f) ? (m.cwndBytes * 8.0f / m.sRttSeconds) : 0.0f;
    m.lossEpoch = tx_->isLossEpoch(ssrc);
    return m;
}

}  // namespace screamtest
