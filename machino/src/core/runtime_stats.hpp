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

    // Platform bring-up failures. A failed bring-up used to be invisible
    // except in the log, and the log is on tmpfs - the OOM of 2026-09-22 was
    // reconstructed only because the box happened to still be up. These make
    // the same facts survive in /api/v1/telemetry.
    uint64_t init_failures = 0;        // bring-up attempts that failed
    uint64_t init_retries = 0;         // immediate retries of an unchanged state;
                                       // MUST stay 0 - the five-retry loop that
                                       // amplified a single failure into an OOM
                                       // was removed, and this proves it stays gone
    int      last_init_rc = 0;         // the vendor return code, verbatim
    // Which stage failed. Free-form rather than an enum because the adapter
    // owns the stage names and the core must not need to know them.
    char     last_init_stage[24] = {0};
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

    // One bring-up attempt failed at `stage` with vendor code `rc`.
    // `stage` is truncated rather than allowed to overflow: a diagnostic must
    // never be the thing that corrupts memory.
    void init_failed(const char* stage, int rc) {
        std::lock_guard<std::mutex> lk(m_);
        ++c_.init_failures;
        c_.last_init_rc = rc;
        size_t i = 0;
        if (stage) for (; stage[i] && i + 1 < sizeof c_.last_init_stage; ++i) c_.last_init_stage[i] = stage[i];
        c_.last_init_stage[i] = 0;
    }

private:
    mutable std::mutex m_;
    RuntimeCounters    c_;
};

} // namespace machino
