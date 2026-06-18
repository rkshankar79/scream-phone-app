#pragma once

#include <cstdint>

#include "../interfaces/FeedbackReceiver.h"

// Forward declaration of the vendored SCReAM receiver (global namespace).
class ScreamRx;

namespace screamtest {

/**
 * Concrete FeedbackReceiver backed by Ericsson's ScreamRx. Confines the
 * scream_core include to the .cpp so the wrapper layer stays engine-free.
 * Not internally synchronized; ReceiverSession serializes access.
 */
class ScreamReceiver : public FeedbackReceiver {
   public:
    explicit ScreamReceiver(uint32_t ssrc);
    ~ScreamReceiver() override;

    void onRtpPacket(uint32_t timeNtp, uint32_t ssrc, int size, uint16_t seqNr,
                     uint8_t ceBits, bool isMarker,
                     uint32_t rtpTimestamp) override;
    int buildFeedback(uint32_t timeNtp, bool isMark, uint8_t* buf,
                      int capacity) override;
    ReceiverMetrics getMetrics() override;

   private:
    ScreamRx* rx_ = nullptr;
    uint64_t rtpReceived_ = 0;
    uint64_t feedbackSent_ = 0;
    uint64_t ceMarked_ = 0;
};

}  // namespace screamtest
