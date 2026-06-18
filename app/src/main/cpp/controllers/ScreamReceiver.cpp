#include "ScreamReceiver.h"

#include "../scream_core/ScreamRx.h"

namespace screamtest {

// createStandardizedFeedback() writes up to ~kMaxRtcpSize (+headroom) bytes.
static constexpr int kMinFeedbackCapacity = 1100;

ScreamReceiver::ScreamReceiver(uint32_t ssrc) { rx_ = new ScreamRx(ssrc); }

ScreamReceiver::~ScreamReceiver() { delete rx_; }

void ScreamReceiver::onRtpPacket(uint32_t timeNtp, uint32_t ssrc, int size,
                                 uint16_t seqNr, uint8_t ceBits, bool isMarker,
                                 uint32_t rtpTimestamp) {
    rtpReceived_++;
    if (ceBits == 0x03) {
        ceMarked_++;
    }
    // ScreamRx ignores the packet payload pointer; it only needs metadata.
    rx_->receive(timeNtp, nullptr, ssrc, size, seqNr, ceBits, isMarker,
                 rtpTimestamp);
}

int ScreamReceiver::buildFeedback(uint32_t timeNtp, bool isMark, uint8_t* buf,
                                  int capacity) {
    if (capacity < kMinFeedbackCapacity) {
        return -1;
    }
    int size = 0;
    bool ok = rx_->createStandardizedFeedback(
        timeNtp, isMark, reinterpret_cast<unsigned char*>(buf), size);
    if (!ok || size <= 0) {
        return -1;  // nothing due yet
    }
    feedbackSent_++;
    return size;
}

ReceiverMetrics ScreamReceiver::getMetrics() {
    ReceiverMetrics m;
    m.rtpReceived = rtpReceived_;
    m.feedbackSent = feedbackSent_;
    m.ceMarked = ceMarked_;
    m.receivedRateBps = rx_->averageReceivedRate;
    return m;
}

}  // namespace screamtest
