#include "TestSession.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>

#include "../controllers/ScreamController.h"

namespace screamtest {

namespace {
constexpr int kRtpHeaderSize = 12;
constexpr int kMaxPacket = 2048;
}  // namespace

TestSession::~TestSession() { stop(); }

double TestSession::nowSeconds() const {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    double t = ts.tv_sec + ts.tv_nsec * 1e-9;
    return t - t0_;
}

uint32_t TestSession::nowNtp() const {
    double t = nowSeconds();
    uint64_t ntp64 = static_cast<uint64_t>(t * 65536.0);
    return static_cast<uint32_t>(0xFFFFFFFF & ntp64);
}

int TestSession::buildRtpHeader(uint8_t* buf, uint16_t seq, uint32_t timestamp,
                                bool marker) const {
    buf[0] = 0x80;  // version 2, no padding/extension/CSRC
    buf[1] = static_cast<uint8_t>((cfg_.payloadType & 0x7F) | (marker ? 0x80 : 0));
    buf[2] = static_cast<uint8_t>((seq >> 8) & 0xFF);
    buf[3] = static_cast<uint8_t>(seq & 0xFF);
    buf[4] = static_cast<uint8_t>((timestamp >> 24) & 0xFF);
    buf[5] = static_cast<uint8_t>((timestamp >> 16) & 0xFF);
    buf[6] = static_cast<uint8_t>((timestamp >> 8) & 0xFF);
    buf[7] = static_cast<uint8_t>(timestamp & 0xFF);
    buf[8] = static_cast<uint8_t>((cfg_.ssrc >> 24) & 0xFF);
    buf[9] = static_cast<uint8_t>((cfg_.ssrc >> 16) & 0xFF);
    buf[10] = static_cast<uint8_t>((cfg_.ssrc >> 8) & 0xFF);
    buf[11] = static_cast<uint8_t>(cfg_.ssrc & 0xFF);
    return kRtpHeaderSize;
}

bool TestSession::openSocket(std::string& err) {
    fd_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd_ < 0) {
        err = std::string("socket() failed: ") + std::strerror(errno);
        return false;
    }

    int reuse = 1;
    setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    // Bind local port (0 => ephemeral) so RTCP feedback can return to us.
    struct sockaddr_in local;
    std::memset(&local, 0, sizeof(local));
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    local.sin_port = htons(static_cast<uint16_t>(cfg_.localPort));
    if (bind(fd_, reinterpret_cast<struct sockaddr*>(&local), sizeof(local)) <
        0) {
        err = std::string("bind() failed: ") + std::strerror(errno);
        close(fd_);
        fd_ = -1;
        return false;
    }

    // ECN codepoint on outgoing packets (IP_TOS lower 2 bits).
    //   ect: -1 Not-ECT, 0 ECT(0)=0b10, 1 ECT(1)=0b01.
    if (cfg_.ect == 0 || cfg_.ect == 1) {
        int tos = (cfg_.ect == 0) ? 0x02 : 0x01;
        if (setsockopt(fd_, IPPROTO_IP, IP_TOS, &tos, sizeof(tos)) < 0) {
            // Non-fatal: keep the session ECN-ready but log-and-continue.
            fprintf(stderr, "TestSession: IP_TOS set failed: %s\n",
                    std::strerror(errno));
        }
    }

    // Receive timeout so rtcpLoop can observe the stop flag.
    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 200000;  // 200 ms
    setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    return true;
}

bool TestSession::start(const SessionConfig& cfg, std::string& err) {
    if (running_.load()) {
        err = "session already running";
        return false;
    }
    cfg_ = cfg;

    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    t0_ = ts.tv_sec + ts.tv_nsec * 1e-9 - 1e-3;

    // Resolve and validate the destination once, so a malformed IP fails loudly
    // instead of silently defaulting to 0.0.0.0.
    std::memset(&dstAddr_, 0, sizeof(dstAddr_));
    dstAddr_.sin_family = AF_INET;
    dstAddr_.sin_port = htons(static_cast<uint16_t>(cfg_.serverPort));
    if (inet_pton(AF_INET, cfg_.serverIp.c_str(), &dstAddr_.sin_addr) != 1) {
        err = "invalid server IP: " + cfg_.serverIp;
        return false;
    }

    if (!openSocket(err)) {
        return false;
    }

    ScreamConfig scfg;
    scfg.queueDelayTargetSeconds = cfg_.delayTargetSeconds;
    scfg.initRateKbps = cfg_.startRateKbps;
    scfg.l4s = (cfg_.ect == 1);
    cc_ = std::make_unique<ScreamController>(scfg);
    cc_->registerStream(cfg_.ssrc, 1.0f, cfg_.minRateKbps * 1000.0f,
                        cfg_.startRateKbps * 1000.0f, cfg_.maxRateKbps * 1000.0f);

    packetsSent_ = 0;
    bytesSent_ = 0;
    feedbackPackets_ = 0;
    seqNr_ = 0;
    rtpTimestamp_ = 0;

    running_ = true;
    txThread_ = std::thread(&TestSession::txLoop, this);
    rtcpThread_ = std::thread(&TestSession::rtcpLoop, this);
    return true;
}

void TestSession::stop() {
    if (!running_.exchange(false)) {
        return;
    }
    if (txThread_.joinable()) {
        txThread_.join();
    }
    if (rtcpThread_.joinable()) {
        rtcpThread_.join();
    }
    if (fd_ >= 0) {
        close(fd_);
        fd_ = -1;
    }
    cc_.reset();
}

void TestSession::generateFrame(double nowSec) {
    const int payloadSize = std::max(64, cfg_.mtu - kRtpHeaderSize);
    uint8_t pkt[kMaxPacket];

    std::lock_guard<std::mutex> lock(ccMutex_);
    float targetBps = cc_->getTargetBitrate(nowNtp(), cfg_.ssrc);
    if (targetBps <= 0.0f) {
        targetBps = cfg_.startRateKbps * 1000.0f;
    }
    int frameBytes =
        std::max(payloadSize,
                 static_cast<int>(targetBps / 8.0f / std::max(1, cfg_.fps)));

    int remaining = frameBytes;
    while (remaining > 0 && running_.load()) {
        int psize = std::min(payloadSize, remaining);
        int total = psize + kRtpHeaderSize;
        if (total > kMaxPacket) {
            total = kMaxPacket;
            psize = total - kRtpHeaderSize;
        }
        bool marker = (remaining - psize) <= 0;
        buildRtpHeader(pkt, seqNr_, rtpTimestamp_, marker);
        std::memset(pkt + kRtpHeaderSize, 0xA5, static_cast<size_t>(psize));

        if (cc_->enqueueRtp(pkt, total, cfg_.ssrc, seqNr_, marker, rtpTimestamp_,
                            static_cast<float>(nowSec))) {
            cc_->newMediaFrame(nowNtp(), cfg_.ssrc, total, marker);
        }
        seqNr_++;
        remaining -= psize;
    }
    // 90 kHz RTP clock advance per frame.
    rtpTimestamp_ += static_cast<uint32_t>(90000 / std::max(1, cfg_.fps));
}

void TestSession::pump() {
    uint8_t out[kMaxPacket];
    for (int i = 0; i < 64 && running_.load(); i++) {
        uint32_t ssrc = cfg_.ssrc;
        int n = -1;
        uint16_t sq = 0;
        bool mk = false;
        uint32_t ts = 0;
        float qd = 0.0f;
        {
            std::lock_guard<std::mutex> lock(ccMutex_);
            float r = cc_->isOkToTransmit(nowNtp(), ssrc);
            if (r != 0.0f) {
                break;  // not ok to transmit (wait) or nothing queued
            }
            n = cc_->dequeueRtp(ssrc, out, sizeof(out),
                                static_cast<float>(nowSeconds()), sq, mk, ts, qd);
        }
        if (n <= 0) {
            break;
        }
        sendto(fd_, out, static_cast<size_t>(n), 0,
               reinterpret_cast<struct sockaddr*>(&dstAddr_), sizeof(dstAddr_));
        packetsSent_++;
        bytesSent_ += static_cast<uint64_t>(n);
        {
            std::lock_guard<std::mutex> lock(ccMutex_);
            cc_->addTransmitted(nowNtp(), ssrc, n, sq, mk, qd, ts);
        }
    }
}

void TestSession::txLoop() {
    const double frameInterval = 1.0 / std::max(1, cfg_.fps);
    double nextFrame = nowSeconds();
    while (running_.load()) {
        double now = nowSeconds();
        if (now >= nextFrame) {
            generateFrame(now);
            nextFrame += frameInterval;
            // Avoid runaway catch-up if we fell far behind.
            if (now - nextFrame > frameInterval) {
                nextFrame = now + frameInterval;
            }
        }
        pump();
        usleep(500);  // 0.5 ms pacing granularity
    }
}

void TestSession::rtcpLoop() {
    uint8_t buf[kMaxPacket];
    while (running_.load()) {
        ssize_t n = recvfrom(fd_, buf, sizeof(buf), 0, nullptr, nullptr);
        if (n <= 0) {
            continue;  // timeout or transient error
        }
        feedbackPackets_++;
        std::lock_guard<std::mutex> lock(ccMutex_);
        cc_->incomingFeedback(nowNtp(), buf, static_cast<int>(n));
    }
}

MetricsSnapshot TestSession::snapshot() {
    MetricsSnapshot m;
    {
        std::lock_guard<std::mutex> lock(ccMutex_);
        if (cc_) {
            m = cc_->getMetrics(nowNtp(), cfg_.ssrc);
        }
    }
    m.packetsSent = packetsSent_.load();
    m.bytesSent = bytesSent_.load();
    m.feedbackPackets = feedbackPackets_.load();
    m.sessionTimeSeconds = nowSeconds();
    return m;
}

}  // namespace screamtest
