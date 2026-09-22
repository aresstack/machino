// One WebRTC session = one UDP socket owned by the poll loop, glued from the
// vector-tested modules: ICE-lite STUN responder (learns the peer address),
// passive DTLS (mbedTLS) whose exporter keys feed Machino's SRTP, and the
// RFC 6184 packetizer fed straight from StreamHub access units. RFC 5764
// demux by first byte: 0..3 STUN, 20..63 DTLS, 128..191 SRTCP. The browser
// sends no RTP on a sendonly track - inbound media traffic is its RTCP,
// which matters for exactly one thing: PLI -> on-demand IDR.
#pragma once
#include "app/webrtc/dtls.hpp"
#include "app/webrtc/rtp.hpp"
#include "app/webrtc/sdp.hpp"
#include <cstdint>
#include <memory>
#include <string>

namespace machino { namespace webrtc {

class PeerSession {
public:
    // `host_ip` is the address the browser reached the camera under; it
    // becomes the one host candidate in the answer.
    explicit PeerSession(const std::string& host_ip);
    ~PeerSession();
    PeerSession(const PeerSession&) = delete;
    PeerSession& operator=(const PeerSession&) = delete;

    bool ok() const { return sock_ >= 0 && dtls_.ok(); }
    int  fd() const { return sock_; }

    // Browser offer in, camera answer out ("" = refuse; `error` says why).
    std::string on_offer(const std::string& offer_sdp, std::string& error);

    // The UDP socket is readable: drain and demux.
    void on_readable();
    // Periodic: DTLS retransmissions and queued handshake flights.
    void tick();

    // DTLS finished and SRTP keys are live.
    bool media_ready() const { return srtp_ != nullptr; }
    // One H.264 access unit (Annex-B) from the hub. Drops whole AUs on
    // backpressure and resumes at the next key frame.
    void send_au(const uint8_t* p, size_t n, int64_t pts_us, bool key);
    // Edge-triggered: a PLI arrived since the last call.
    bool take_pli();
    // Compact media-plane fault localisation, logged ~every 2 s by the caller.
    void log_stats();

private:
    void flush_dtls();
    bool send_udp(const uint8_t* p, size_t n);

    int         sock_ = -1;
    std::string ufrag_, pwd_;          // our ICE credentials
    DtlsTransport dtls_;
    std::unique_ptr<SrtpSession> srtp_;
    RtpParams   rtp_;
    uint16_t    seq_ = 0;
    bool        await_key_ = true;
    bool        pli_ = false;
    bool        offered_ = false;
    // learned from the first authenticated STUN binding request
    uint32_t    peer_ip_ = 0;          // host order
    uint16_t    peer_port_ = 0;
    bool        have_peer_ = false;
    // media-plane telemetry (logged periodically): the exact fault localisation
    // GPT asked for - where the H.264 stops on its way to the browser.
    bool        dtls_logged_ = false;
    bool        dtls_hexdumped_ = false;
    uint64_t    stun_reqs_ = 0;
    uint64_t    au_count_ = 0, rtp_count_ = 0, rtp_bytes_ = 0;
    uint64_t    send_ok_ = 0, send_err_ = 0;
    int         last_send_errno_ = 0;
    uint64_t    rtcp_in_ = 0, pli_in_ = 0;
    int64_t     last_stat_ms_ = 0;
};

}} // namespace machino::webrtc
