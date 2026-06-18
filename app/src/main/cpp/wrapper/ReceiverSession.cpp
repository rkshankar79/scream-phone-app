#include "ReceiverSession.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

#include "../controllers/ScreamReceiver.h"

namespace screamtest {

namespace {
constexpr int kRtpHeaderSize = 12;
constexpr int kBufSize = 2048;
}  // namespace

ReceiverSession::~ReceiverSession() { stop(); }

double ReceiverSession::nowSeconds() const {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (ts.tv_sec + ts.tv_nsec * 1e-9) - t0_;
}

uint32_t ReceiverSession::nowNtp() const {
    uint64_t ntp64 = static_cast<uint64_t>(nowSeconds() * 65536.0);
    return static_cast<uint32_t>(0xFFFFFFFF & ntp64);
}

bool ReceiverSession::start(const ReceiverConfig& cfg, std::string& err) {
    if (running_.load()) {
        err = "receiver already running";
        return false;
    }
    cfg_ = cfg;

    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    t0_ = ts.tv_sec + ts.tv_nsec * 1e-9 - 1e-3;

    fd_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd_ < 0) {
        err = std::string("socket() failed: ") + std::strerror(errno);
        return false;
    }
    int reuse = 1;
    setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in local;
    std::memset(&local, 0, sizeof(local));
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    local.sin_port = htons(static_cast<uint16_t>(cfg_.listenPort));
    if (bind(fd_, reinterpret_cast<struct sockaddr*>(&local), sizeof(local)) <
        0) {
        err = std::string("bind() failed: ") + std::strerror(errno);
        close(fd_);
        fd_ = -1;
        return false;
    }

    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 200000;  // 200 ms so rxLoop can observe stop()
    setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    rx_ = std::make_unique<ScreamReceiver>(cfg_.ssrc);

    running_ = true;
    rxThread_ = std::thread(&ReceiverSession::rxLoop, this);
    return true;
}

void ReceiverSession::stop() {
    if (!running_.exchange(false)) {
        return;
    }
    if (rxThread_.joinable()) {
        rxThread_.join();
    }
    if (fd_ >= 0) {
        close(fd_);
        fd_ = -1;
    }
    rx_.reset();
}

void ReceiverSession::rxLoop() {
    uint8_t buf[kBufSize];
    uint8_t fb[kBufSize];
    struct sockaddr_in src;

    while (running_.load()) {
        socklen_t srcLen = sizeof(src);
        ssize_t n = recvfrom(fd_, buf, sizeof(buf), 0,
                             reinterpret_cast<struct sockaddr*>(&src), &srcLen);
        if (n < kRtpHeaderSize) {
            continue;  // timeout or non-RTP
        }

        // Parse the minimal RTP header.
        bool marker = (buf[1] & 0x80) != 0;
        uint16_t seq = (static_cast<uint16_t>(buf[2]) << 8) | buf[3];
        uint32_t tsRtp = (static_cast<uint32_t>(buf[4]) << 24) |
                         (static_cast<uint32_t>(buf[5]) << 16) |
                         (static_cast<uint32_t>(buf[6]) << 8) | buf[7];
        uint32_t ssrc = (static_cast<uint32_t>(buf[8]) << 24) |
                        (static_cast<uint32_t>(buf[9]) << 16) |
                        (static_cast<uint32_t>(buf[10]) << 8) | buf[11];
        // ECN CE bits: 0 in the MVP (no recvmsg/cmsg yet) -> ECN-ready.
        uint8_t ceBits = 0;

        int fbSize = -1;
        {
            std::lock_guard<std::mutex> lock(rxMutex_);
            rx_->onRtpPacket(nowNtp(), ssrc, static_cast<int>(n), seq, ceBits,
                             marker, tsRtp);
            fbSize = rx_->buildFeedback(nowNtp(), marker, fb, sizeof(fb));
        }
        if (fbSize > 0) {
            sendto(fd_, fb, static_cast<size_t>(fbSize), 0,
                   reinterpret_cast<struct sockaddr*>(&src), srcLen);
        }
    }
}

ReceiverMetrics ReceiverSession::snapshot() {
    ReceiverMetrics m;
    {
        std::lock_guard<std::mutex> lock(rxMutex_);
        if (rx_) {
            m = rx_->getMetrics();
        }
    }
    m.sessionTimeSeconds = nowSeconds();
    return m;
}

}  // namespace screamtest
