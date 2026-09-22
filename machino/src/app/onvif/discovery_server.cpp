#include "app/onvif/discovery_server.hpp"
#include "core/log.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <ctime>
#include <unistd.h>

namespace machino { namespace onvif {

static const char* MOD = "WSDISC";
static const char* GROUP = "239.255.255.250";
static const int   PORT = 3702;
// A Probe is a few hundred bytes. Anything larger is not one, and reading it
// into a bigger buffer would only give a hostile sender a bigger target.
static const size_t MAX_DATAGRAM = 8192;

DiscoveryServer::DiscoveryServer(std::string uuid_seed, std::string scopes, int http_port)
    : uuid_(device_uuid(uuid_seed)), scopes_(std::move(scopes)), http_port_(http_port) {}

DiscoveryServer::~DiscoveryServer() { stop(); }

Announcement DiscoveryServer::announcement_for(const std::string& local_ip) const {
    Announcement a;
    a.uuid = uuid_;
    a.scopes = scopes_;
    char x[256];
    snprintf(x, sizeof x, "http://%s:%d/onvif/device_service", local_ip.c_str(), http_port_);
    a.xaddr = x;
    return a;
}

std::string DiscoveryServer::local_address_for(const void* peer_sockaddr, unsigned peer_len) const {
    // Connect a throwaway UDP socket to the peer and ask the kernel which
    // source address it picked. No packet is sent; this is purely a routing
    // question, and it is the only answer that is right on a multi-homed box.
    const int s = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (s < 0) return "0.0.0.0";
    std::string out = "0.0.0.0";
    if (connect(s, (const struct sockaddr*)peer_sockaddr, (socklen_t)peer_len) == 0) {
        struct sockaddr_in me{};
        socklen_t len = sizeof me;
        if (getsockname(s, (struct sockaddr*)&me, &len) == 0) {
            char buf[INET_ADDRSTRLEN] = {0};
            if (inet_ntop(AF_INET, &me.sin_addr, buf, sizeof buf)) out = buf;
        }
    }
    close(s);
    return out;
}

Result DiscoveryServer::start() {
    if (running_.load()) return Result::busy();

    fd_ = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd_ < 0) { LOGE(MOD, "socket: %s", strerror(errno)); return Result::error(errno); }

    // 3702 is a well-known multicast port that other discovery daemons may
    // also want; refusing to share it would make this an all-or-nothing
    // conflict with whatever else is on the camera.
    int one = 1;
    setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
#ifdef SO_REUSEPORT
    setsockopt(fd_, SOL_SOCKET, SO_REUSEPORT, &one, sizeof one);
#endif

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(PORT);
    if (bind(fd_, (struct sockaddr*)&addr, sizeof addr) != 0) {
        LOGE(MOD, "bind %d: %s", PORT, strerror(errno));
        close(fd_); fd_ = -1;
        return Result::error(errno);
    }

    struct ip_mreq mreq{};
    mreq.imr_multiaddr.s_addr = inet_addr(GROUP);
    mreq.imr_interface.s_addr = htonl(INADDR_ANY);
    if (setsockopt(fd_, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof mreq) != 0) {
        // Not fatal: a unicast Probe still reaches us, which is how some
        // clients ask once they know the address.
        LOGW(MOD, "multicast join failed (%s) - unicast probes only", strerror(errno));
    }

    if (pipe(wake_) != 0) {
        LOGE(MOD, "pipe: %s", strerror(errno));
        close(fd_); fd_ = -1;
        return Result::error(errno);
    }
    fcntl(wake_[0], F_SETFL, O_NONBLOCK);
    fcntl(wake_[0], F_SETFD, FD_CLOEXEC);
    fcntl(wake_[1], F_SETFD, FD_CLOEXEC);

    running_.store(true);
    th_ = std::thread([this] { loop(); });
    LOGI(MOD, "ws-discovery on %s:%d, endpoint %s", GROUP, PORT, uuid_.c_str());
    return Result::ok();
}

void DiscoveryServer::stop() {
    if (!running_.exchange(false)) return;
    if (wake_[1] >= 0) { const char b = 1; ssize_t n = write(wake_[1], &b, 1); (void)n; }
    if (th_.joinable()) th_.join();
    for (int* p : {&wake_[0], &wake_[1]}) { if (*p >= 0) { close(*p); *p = -1; } }
    if (fd_ >= 0) { close(fd_); fd_ = -1; }
    LOGI(MOD, "ws-discovery stopped (answered=%llu ignored=%llu)",
         (unsigned long long)answered_.load(), (unsigned long long)ignored_.load());
}

void DiscoveryServer::loop() {
    std::string buf;
    buf.resize(MAX_DATAGRAM);
    while (running_.load()) {
        struct pollfd pfd[2];
        pfd[0].fd = fd_;      pfd[0].events = POLLIN; pfd[0].revents = 0;
        pfd[1].fd = wake_[0]; pfd[1].events = POLLIN; pfd[1].revents = 0;
        const int n = poll(pfd, 2, 1000);
        if (n < 0) { if (errno == EINTR) continue; break; }
        if (pfd[1].revents & POLLIN) break;          // stop()
        if (!(pfd[0].revents & POLLIN)) continue;

        struct sockaddr_in peer{};
        socklen_t plen = sizeof peer;
        const ssize_t got = recvfrom(fd_, &buf[0], buf.size(), 0,
                                     (struct sockaddr*)&peer, &plen);
        if (got <= 0) continue;

        Probe p;
        if (!parse_probe(std::string(buf.data(), (size_t)got), p)) continue;   // not a Probe
        if (!probe_matches_us(p)) {
            // Answering a Probe for a type this camera is not would put it in
            // a list it does not belong in.
            ignored_.fetch_add(1);
            continue;
        }

        const std::string local = local_address_for(&peer, plen);
        const std::string reply = probe_matches(announcement_for(local), p.message_id,
                                                message_id(++msg_counter_, (int64_t)::time(nullptr)));
        // The reply is UNICAST back to the sender, per the specification: a
        // multicast answer would be seen by every client on the segment as an
        // answer to a question it did not ask.
        const ssize_t sent = sendto(fd_, reply.data(), reply.size(), 0,
                                    (struct sockaddr*)&peer, plen);
        if (sent < 0) LOGW(MOD, "reply failed: %s", strerror(errno));
        else answered_.fetch_add(1);
    }
}

}} // namespace machino::onvif
