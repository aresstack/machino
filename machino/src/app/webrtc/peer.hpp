// One WebRTC session = one UDP socket owned by the poll loop, glued from the
// vector-tested modules: ICE-lite STUN responder (learns the peer address),
// passive DTLS (mbedTLS) whose exporter keys feed Machino's SRTP, and the
// RFC 6184 packetizer fed straight from StreamHub access units. RFC 5764
// demux by first byte: 0..3 STUN, 20..63 DTLS, 128..191 SRTP/SRTCP. Inbound
// RTCP matters for one thing: PLI -> on-demand IDR. Inbound RTP is talkback:
// G.711 from the browser's microphone, decoded here to PCM for the speaker.
// Outbound audio is the camera's microphone as G.711 on its own SSRC.
#pragma once
#include "app/webrtc/dtls.hpp"
#include "app/webrtc/rtp.hpp"
#include "app/webrtc/sdp.hpp"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

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
    // `local_profile` is the profile-level-id this camera really emits (from
    // the SPS, e.g. "640033"); an offer that contains it is answered with it
    // instead of an approximation. Empty = follow the browser's preference.
    // `audio_send` / `audio_recv`: may this session carry the microphone out /
    // talkback in? The offer's own direction narrows it (plan_audio).
    std::string on_offer(const std::string& offer_sdp, std::string& error,
                         const std::string& local_profile = "",
                         bool audio_send = false, bool audio_recv = false);

    // What the answer settled for audio.
    bool audio_sending() const { return audio_.send; }
    bool audio_receiving() const { return audio_.recv; }
    bool audio_is_pcma() const { return audio_pcma_; }
    // One chunk of G.711 (8 kHz, one byte per sample) from the microphone.
    // skip_samples: G.711 samples (8 kHz) for sink-dropped frames since the
    // last send, so the RTP timestamp skips the gap instead of pretending the
    // dropped frames' airtime never passed.
    void send_audio(const uint8_t* g711, size_t n, uint32_t skip_samples = 0);
    // Talkback received since the last call, as 8 kHz PCM. False when none.
    bool take_audio_in(std::vector<int16_t>& pcm);

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

    // The media socket can no longer reach the browser and will not recover
    // on its own: every recent send failed with an errno that says the PATH
    // is gone, not the buffer. Measured 2026-09-29: the cellular uplink got a
    // new carrier address, the UDP socket stayed bound to the old one, and
    // sendto() answered EINVAL for every packet -- 959 times before anybody
    // looked. The browser only noticed minutes later. A session that reads
    // stranded() is closed by the owner so the client reconnects at once.
    bool stranded() const { return fatal_send_streak_ >= kStrandedAfter; }
    // Errnos that mean "the socket's source address or route is gone" rather
    // than "the buffer is full". Pure, so the host tests can pin the list.
    static bool is_fatal_send_errno(int e);
    static const unsigned kStrandedAfter = 25;

private:
    void flush_dtls();
    bool send_udp(const uint8_t* p, size_t n);

    int         sock_ = -1;
    std::string ufrag_, pwd_;          // our ICE credentials
    DtlsTransport dtls_;
    std::unique_ptr<SrtpSession> srtp_;
    RtpParams   rtp_;
    uint16_t    seq_ = 0;
    AudioPlan   audio_;
    bool        audio_pcma_ = true;    // the negotiated payload is PCMA (else PCMU)
    uint32_t    audio_ssrc_ = 0;
    uint16_t    audio_seq_ = 0;
    uint32_t    audio_ts_ = 0;
    std::vector<int16_t> audio_in_;    // decoded talkback, bounded (1 s)
    uint64_t    audio_out_pkts_ = 0, audio_in_pkts_ = 0;
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
    uint64_t    stun_reqs_ = 0;
    uint64_t    au_count_ = 0, rtp_count_ = 0, rtp_bytes_ = 0;
    uint64_t    send_ok_ = 0, send_err_ = 0;
    int         last_send_errno_ = 0;
    unsigned    fatal_send_streak_ = 0;  // consecutive sends failing with a fatal errno
    uint64_t    rtcp_in_ = 0, pli_in_ = 0;
    int64_t     last_stat_ms_ = 0;
};

}} // namespace machino::webrtc
