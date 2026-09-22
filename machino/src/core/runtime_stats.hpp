// Process-wide runtime counters for diagnostics (AP5).
//
// Deliberately small: it holds ONLY what no subsystem already reports.
// Pipeline starts/stops/restarts come from lifecycle::Stats, per-unit encoded
// fps/bytes/drops from Measurement - duplicating those here would create two
// numbers that can disagree. What is missing without this is the network side:
// how many sessions are live and how the WebRTC media plane is faring.
//
// Counters are monotonic; gauges are current values. Everything is relaxed
// atomics: a diagnostic must never add ordering to the media path.
#pragma once
#include <atomic>
#include <cstdint>

namespace machino {

struct RuntimeStats {
    // gauges
    std::atomic<int>      rtsp_sessions{0};       // connections currently PLAYing
    std::atomic<int>      webrtc_sessions{0};     // negotiated peer sessions
    std::atomic<int>      ws_video_clients{0};    // MSE viewers
    std::atomic<int>      ws_logs_clients{0};     // log viewers

    // monotonic counters
    std::atomic<uint64_t> webrtc_dtls_failures{0};
    std::atomic<uint64_t> webrtc_srtp_failures{0};   // key export refused / protect failed
    std::atomic<uint64_t> webrtc_rtp_packets{0};
    std::atomic<uint64_t> webrtc_rtp_bytes{0};
    std::atomic<uint64_t> webrtc_send_errors{0};
    std::atomic<uint64_t> webrtc_pli{0};             // picture-loss requests honoured
    std::atomic<uint64_t> streamhub_dropped_aus{0};  // access units a slow consumer lost

    static RuntimeStats& get();

    // convenience: relaxed because none of these order anything
    static void inc(std::atomic<uint64_t>& c, uint64_t n = 1) { c.fetch_add(n, std::memory_order_relaxed); }
    static void inc(std::atomic<int>& g) { g.fetch_add(1, std::memory_order_relaxed); }
    static void dec(std::atomic<int>& g) { g.fetch_sub(1, std::memory_order_relaxed); }
};

} // namespace machino
