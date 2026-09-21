#include "app/webrtc/peer.hpp"
#include "app/webrtc/stun.hpp"
#include "core/log.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

static const char* MOD = "RTC";

namespace machino { namespace webrtc {

namespace {

std::string random_token(size_t n) {
    static const char* T = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    uint8_t raw[64];
    size_t got = 0;
    FILE* f = fopen("/dev/urandom", "rb");
    if (f) { got = fread(raw, 1, n > 64 ? 64 : n, f); fclose(f); }
    std::string s;
    for (size_t i = 0; i < n; ++i) s += T[(got > i ? raw[i] : (uint8_t)(i * 37 + 11)) % 62];
    return s;
}

} // namespace

PeerSession::PeerSession(const std::string& host_ip) {
    ufrag_ = random_token(8);
    pwd_ = random_token(24);
    rtp_.ssrc = 0;
    uint8_t r[4] = {0, 0, 0, 0};
    FILE* f = fopen("/dev/urandom", "rb");
    if (f) { if (fread(r, 1, 4, f) != 4) { /* fallback below */ } fclose(f); }
    rtp_.ssrc = ((uint32_t)r[0] << 24) | ((uint32_t)r[1] << 16) | ((uint32_t)r[2] << 8) | r[3];
    if (!rtp_.ssrc) rtp_.ssrc = 0x4d414348;                     // "MACH"
    seq_ = (uint16_t)(rtp_.ssrc >> 8);

    sock_ = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (sock_ < 0) return;
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = 0;                                             // ephemeral
    if (inet_pton(AF_INET, host_ip.c_str(), &a.sin_addr) != 1)
        a.sin_addr.s_addr = INADDR_ANY;
    if (bind(sock_, (sockaddr*)&a, sizeof a) < 0) { close(sock_); sock_ = -1; return; }
    int sz = 512 * 1024;                                        // an IDR burst fits
    setsockopt(sock_, SOL_SOCKET, SO_SNDBUF, &sz, sizeof sz);
}

PeerSession::~PeerSession() {
    if (sock_ >= 0) close(sock_);
}

std::string PeerSession::on_offer(const std::string& offer_sdp, std::string& error) {
    if (!ok()) { error = "session setup failed"; return ""; }
    if (offered_) { error = "already offered"; return ""; }
    Offer o = parse_offer(offer_sdp);
    if (!o.ok) { error = o.error; return ""; }
    sockaddr_in a{}; socklen_t al = sizeof a;
    if (getsockname(sock_, (sockaddr*)&a, &al) < 0) { error = "getsockname failed"; return ""; }
    char ip[INET_ADDRSTRLEN] = "0.0.0.0";
    inet_ntop(AF_INET, &a.sin_addr, ip, sizeof ip);
    rtp_.payload_type = (uint8_t)o.media[(size_t)o.video_index].h264_pt;
    AnswerParams p;
    p.ice_ufrag = ufrag_;
    p.ice_pwd = pwd_;
    p.fingerprint = dtls_.fingerprint();
    p.host_ip = ip;
    p.port = ntohs(a.sin_port);
    p.ssrc = rtp_.ssrc;
    offered_ = true;
    LOGI(MOD, "offer accepted: pt=%d candidate %s:%u", rtp_.payload_type, ip, (unsigned)p.port);
    return build_answer(o, p);
}

bool PeerSession::send_udp(const uint8_t* p, size_t n) {
    if (!have_peer_) return false;
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(peer_port_);
    a.sin_addr.s_addr = htonl(peer_ip_);
    return sendto(sock_, p, n, MSG_DONTWAIT, (sockaddr*)&a, sizeof a) == (ssize_t)n;
}

void PeerSession::flush_dtls() {
    std::vector<uint8_t> d;
    while (dtls_.take_out(d)) send_udp(d.data(), d.size());
}

void PeerSession::on_readable() {
    uint8_t buf[2048];
    for (;;) {
        sockaddr_in from{}; socklen_t fl = sizeof from;
        const ssize_t n = recvfrom(sock_, buf, sizeof buf, MSG_DONTWAIT, (sockaddr*)&from, &fl);
        if (n <= 0) {
            if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
                LOGW(MOD, "recvfrom: %s", strerror(errno));
            return;
        }
        const uint8_t b0 = buf[0];
        if (is_stun(buf, (size_t)n)) {
            StunRequest req = parse_binding_request(buf, (size_t)n, pwd_);
            if (!req.ok || !req.integrity_ok) continue;         // not ours / wrong creds
            // the authenticated check IS the peer address discovery (ICE-lite)
            peer_ip_ = ntohl(from.sin_addr.s_addr);
            peer_port_ = ntohs(from.sin_port);
            if (!have_peer_) LOGI(MOD, "peer %u.%u.%u.%u:%u (stun ok)",
                                  (peer_ip_ >> 24) & 255, (peer_ip_ >> 16) & 255,
                                  (peer_ip_ >> 8) & 255, peer_ip_ & 255, peer_port_);
            have_peer_ = true;
            std::vector<uint8_t> resp = binding_response(req.tid, peer_ip_, peer_port_, pwd_);
            send_udp(resp.data(), resp.size());
        } else if (b0 >= 20 && b0 <= 63) {                      // DTLS
            dtls_.feed(buf, (size_t)n);
            if (!dtls_.step()) return;                          // fatal alert: media never starts
            flush_dtls();
            if (dtls_.handshake_done() && !srtp_) {
                SrtpKey ours{}, theirs{};
                if (dtls_.export_srtp(ours, theirs)) {
                    srtp_.reset(new SrtpSession(ours, theirs));
                    await_key_ = true;
                    pli_ = true;                                // start with a fresh IDR
                    LOGI(MOD, "dtls done, srtp live");
                }
            }
        } else if (b0 >= 128 && b0 <= 191) {                    // SRTP/SRTCP
            if (!srtp_) continue;
            std::vector<uint8_t> pkt(buf, buf + n);
            if (is_rtcp(buf, (size_t)n) && srtp_->unprotect_rtcp(pkt)) {
                RtcpInfo info = parse_rtcp(pkt.data(), pkt.size());
                if (info.pli) pli_ = true;
            }
        }
    }
}

void PeerSession::tick() {
    if (!dtls_.handshake_done()) {
        dtls_.step();                                           // drives DTLS retransmission timers
        flush_dtls();
    }
}

bool PeerSession::take_pli() {
    const bool p = pli_;
    pli_ = false;
    return p;
}

void PeerSession::send_au(const uint8_t* p, size_t n, int64_t pts_us, bool key) {
    if (!srtp_ || !have_peer_) return;
    if (await_key_ && !key) return;
    const uint32_t ts = (uint32_t)((uint64_t)pts_us * 9 / 100); // us -> 90 kHz
    auto pkts = packetize_h264(p, n, ts, seq_, rtp_);
    for (auto& pkt : pkts) {
        if (!srtp_->protect_rtp(pkt)) return;
        if (!send_udp(pkt.data(), pkt.size())) {
            // socket backpressure: drop the rest of this AU, resume at a key
            await_key_ = true;
            return;
        }
    }
    await_key_ = false;
}

}} // namespace machino::webrtc
