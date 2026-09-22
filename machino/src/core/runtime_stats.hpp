// Process-wide runtime counters for diagnostics (AP5).
//
// Deliberately small: it holds ONLY what no subsystem already reports.
// Pipeline starts/stops/restarts come from lifecycle::Stats, per-unit encoded
// fps/bytes/drops from Measurement - duplicating those here would create two
// numbers that can disagree. What is missing without this is the network side:
// how many sessions are live and how the WebRTC media plane is faring.
//
// Plain integers under a mutex, NOT std::atomic<uint64_t>: MIPS32 has no
// 64-bit lock-free atomics, so those pull in libatomic and fail the cross
// link (this bit the project twice). The busiest counter runs at a few
// hundred increments per second, where a mutex costs nothing - and a
// diagnostic must never be the reason the media path stalls.
#pragma once
#include <cstdint>
#include <mutex>

namespace machino {

struct RuntimeCounters {
    // gauges
    int      rtsp_sessions = 0;        // connections currently PLAYing
    int      webrtc_sessions = 0;      // negotiated peer sessions
    int      ws_video_clients = 0;     // MSE viewers
    int      ws_logs_clients = 0;      // log viewers
    // monotonic
    uint64_t webrtc_dtls_failures = 0;
    uint64_t webrtc_srtp_failures = 0; // key export refused / protect failed
    uint64_t webrtc_rtp_packets = 0;
    uint64_t webrtc_rtp_bytes = 0;
    uint64_t webrtc_send_errors = 0;
    uint64_t webrtc_pli = 0;           // picture-loss requests honoured
};

class RuntimeStats {
public:
    static RuntimeStats& get();

    RuntimeCounters snapshot() const {
        std::lock_guard<std::mutex> lk(m_);
        return c_;
    }

    // Member pointers keep the call sites readable without exposing the
    // fields for unlocked access.
    void inc(uint64_t RuntimeCounters::* f, uint64_t n = 1) {
        std::lock_guard<std::mutex> lk(m_);
        c_.*f += n;
    }
    void inc(int RuntimeCounters::* g) {
        std::lock_guard<std::mutex> lk(m_);
        ++(c_.*g);
    }
    void dec(int RuntimeCounters::* g) {
        std::lock_guard<std::mutex> lk(m_);
        if (c_.*g > 0) --(c_.*g);      // a gauge must never go negative
    }

private:
    mutable std::mutex m_;
    RuntimeCounters    c_;
};

} // namespace machino
