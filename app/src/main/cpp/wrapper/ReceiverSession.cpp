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

/** Lower 2 bits of IP_TOS / traffic-class cmsg (ECN codepoint). */
uint8_t ecnFromMsghdr(struct msghdr& msg) {
    for (struct cmsghdr* c = CMSG_FIRSTHDR(&msg); c != nullptr;
         c = CMSG_NXTHDR(&msg, c)) {
        if (c->cmsg_level == IPPROTO_IP && c->cmsg_type == IP_TOS &&
            c->cmsg_len >= CMSG_LEN(sizeof(int))) {
            int tos = 0;
            std::memcpy(&tos, CMSG_DATA(c), sizeof(tos));
            return static_cast<uint8_t>(tos & 0x03);
        }
#if defined(IPV6_TCLASS)
        if (c->cmsg_level == IPPROTO_IPV6 && c->cmsg_type == IPV6_TCLASS &&
            c->cmsg_len >= CMSG_LEN(sizeof(int))) {
            int tclass = 0;
            std::memcpy(&tclass, CMSG_DATA(c), sizeof(tclass));
            return static_cast<uint8_t>(tclass & 0x03);
        }
#endif
    }
    return 0;
}
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

    // Receive IP ECN codepoint (ECT/CE) via ancillary data on each datagram.
    int recvtos = 1;
    if (setsockopt(fd_, IPPROTO_IP, IP_RECVTOS, &recvtos, sizeof(recvtos)) < 0) {
        fprintf(stderr, "ReceiverSession: IP_RECVTOS failed: %s\n",
                std::strerror(errno));
    }

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
    struct msghdr msg {};
    struct iovec iov {};
    char ctrl[CMSG_SPACE(sizeof(int))];

    iov.iov_base = buf;
    iov.iov_len = sizeof(buf);
    msg.msg_name = &src;
    msg.msg_namelen = sizeof(src);
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = ctrl;
    msg.msg_controllen = sizeof(ctrl);

    while (running_.load()) {
        msg.msg_namelen = sizeof(src);
        msg.msg_controllen = sizeof(ctrl);
        ssize_t n = recvmsg(fd_, &msg, 0);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            continue;
        }
        if (n < kRtpHeaderSize) {
            continue;
        }

        const uint8_t ceBits = ecnFromMsghdr(msg);

        // Parse the minimal RTP header.
        bool marker = (buf[1] & 0x80) != 0;
        uint16_t seq = (static_cast<uint16_t>(buf[2]) << 8) | buf[3];
        uint32_t tsRtp = (static_cast<uint32_t>(buf[4]) << 24) |
                         (static_cast<uint32_t>(buf[5]) << 16) |
                         (static_cast<uint32_t>(buf[6]) << 8) | buf[7];
        uint32_t ssrc = (static_cast<uint32_t>(buf[8]) << 24) |
                        (static_cast<uint32_t>(buf[9]) << 16) |
                        (static_cast<uint32_t>(buf[10]) << 8) | buf[11];

        int fbSize = -1;
        {
            std::lock_guard<std::mutex> lock(rxMutex_);
            rx_->onRtpPacket(nowNtp(), ssrc, static_cast<int>(n), seq, ceBits,
                             marker, tsRtp);
            fbSize = rx_->buildFeedback(nowNtp(), marker, fb, sizeof(fb));
        }
        if (fbSize > 0) {
            sendto(fd_, fb, static_cast<size_t>(fbSize), 0,
                   reinterpret_cast<struct sockaddr*>(&src), msg.msg_namelen);
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
