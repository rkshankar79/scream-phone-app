#pragma once

#include <netinet/in.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "../interfaces/CongestionController.h"
#include "../interfaces/MetricsSnapshot.h"

namespace screamtest {

/** Runtime configuration for a sender test session. */
struct SessionConfig {
    std::string serverIp = "127.0.0.1";
    int serverPort = 30000;
    int localPort = 0;  // 0 => ephemeral; set a fixed port for Android<->Linux

    int minRateKbps = 1000;
    int startRateKbps = 2000;
    int maxRateKbps = 50000;

    int fps = 50;
    int mtu = 1200;

    // ECN codepoint on transmitted packets:
    //   -1 = Not-ECT, 0 = ECT(0), 1 = ECT(1)/L4S.
    // MVP is ECN-ready but not ECN-dependent: default Not-ECT, user may pick
    // ECT(0). This is the seam to later extract into a dedicated EcnManager.
    int ect = -1;

    float delayTargetSeconds = 0.06f;
    uint32_t ssrc = 100;
    int payloadType = 96;
};

/**
 * Owns the full sender pipeline: a CongestionController (via interface only),
 * a UDP socket, a transmit thread (synthetic traffic generation + SCReAM
 * pacing) and an RTCP receive thread (RFC 8888 feedback).
 *
 * All controller calls are serialized under ccMutex_.
 */
class TestSession {
   public:
    TestSession() = default;
    ~TestSession();

    bool start(const SessionConfig& cfg, std::string& err);
    void stop();
    bool isRunning() const { return running_.load(); }

    MetricsSnapshot snapshot();

   private:
    void txLoop();
    void rtcpLoop();
    void generateFrame(double nowSec);
    void pump();

    bool openSocket(std::string& err);
    int buildRtpHeader(uint8_t* buf, uint16_t seq, uint32_t timestamp,
                       bool marker) const;

    uint32_t nowNtp() const;
    double nowSeconds() const;

    SessionConfig cfg_;
    std::unique_ptr<CongestionController> cc_;
    std::mutex ccMutex_;

    int fd_ = -1;
    struct sockaddr_in dstAddr_{};
    std::thread txThread_;
    std::thread rtcpThread_;
    std::atomic<bool> running_{false};
    double t0_ = 0.0;

    // Transport bookkeeping (read from the metrics thread).
    std::atomic<uint64_t> packetsSent_{0};
    std::atomic<uint64_t> bytesSent_{0};
    std::atomic<uint64_t> feedbackPackets_{0};

    uint16_t seqNr_ = 0;
    uint32_t rtpTimestamp_ = 0;
};

}  // namespace screamtest
