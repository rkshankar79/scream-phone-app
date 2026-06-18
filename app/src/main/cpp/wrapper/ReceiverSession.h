#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "../interfaces/FeedbackReceiver.h"

namespace screamtest {

struct ReceiverConfig {
    int listenPort = 30000;
    uint32_t ssrc = 100;  // informational; the SSRC is read from each RTP packet
};

/**
 * Owns the SCReAM receiver role: a UDP socket that ingests RTP, hands metadata
 * to a FeedbackReceiver (interface only) and returns RFC 8888 RTCP feedback to
 * the packet's source address. Single receive thread; feedback is emitted
 * inline (the engine self-gates the cadence).
 */
class ReceiverSession {
   public:
    ReceiverSession() = default;
    ~ReceiverSession();

    bool start(const ReceiverConfig& cfg, std::string& err);
    void stop();
    bool isRunning() const { return running_.load(); }

    ReceiverMetrics snapshot();

   private:
    void rxLoop();
    uint32_t nowNtp() const;
    double nowSeconds() const;

    ReceiverConfig cfg_;
    std::unique_ptr<FeedbackReceiver> rx_;
    std::mutex rxMutex_;

    int fd_ = -1;
    std::thread rxThread_;
    std::atomic<bool> running_{false};
    double t0_ = 0.0;
};

}  // namespace screamtest
