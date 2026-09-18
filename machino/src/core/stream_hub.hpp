// Machino core: fan-out of access units to N consumers with bounded,
// per-consumer queues and a live-stream stale-frame policy:
//   * old frames are worse than dropped frames - a slow consumer drops its
//     own oldest frames, never blocks the encoder or other consumers;
//   * after any drop the consumer resumes only at the next key frame
//     (discontinuity), so a decoder never sees P-frames whose reference was
//     dropped;
//   * a key frame flushes older queued frames (a decoder can resume from it).
// Also keeps a windowed latency statistic (capture -> encoder out -> send).
#pragma once
#include "core/frame.hpp"
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <vector>

namespace machino {

class Sink {
public:
    explicit Sink(size_t depth) : depth_(depth ? depth : 1) {}
    // Blocks up to timeout_ms; false on timeout or after close().
    // `discontinuity` is set when frames were dropped since the last pop:
    // the consumer must wait for the next key frame.
    bool pop(AuPtr& out, int timeout_ms, bool* discontinuity = nullptr);
    void push(const AuPtr& au);      // called by the hub
    void close();
    bool closed() const { return closed_; }
    unsigned dropped() const { return dropped_; }
    size_t depth() const { return depth_; }
private:
    std::mutex              m_;
    std::condition_variable cv_;
    std::deque<AuPtr>       q_;
    size_t                  depth_;
    bool                    closed_  = false;
    bool                    disc_    = false;
    unsigned                dropped_ = 0;
};

// Windowed latency statistics (microseconds), sampled by the producer and the
// transports. `capture_to_out` = encoder pts -> fetched by Machino (same clock
// domain supplied by the platform); `out_to_send` = fetched -> handed to the socket.
struct LatencyStats {
    bool     valid = false;
    unsigned samples = 0;
    double   capture_to_out_avg_ms = 0, capture_to_out_max_ms = 0;
    double   out_to_send_avg_ms = 0,    out_to_send_max_ms = 0;
    unsigned discontinuities = 0;       // consumer resyncs on key frames
};

class StreamHub {
public:
    std::shared_ptr<Sink> subscribe(size_t depth = 0);      // 0 = current default depth
    void unsubscribe(const std::shared_ptr<Sink>& s);
    void publish(const AuPtr& au);
    size_t consumers() const;

    void   set_default_depth(size_t d) { std::lock_guard<std::mutex> lk(m_); default_depth_ = d ? d : 1; }
    size_t default_depth() const { std::lock_guard<std::mutex> lk(m_); return default_depth_; }

    // latency accounting (called from producer / transport threads)
    void record_capture_to_out(int64_t us);
    void record_out_to_send(int64_t us);
    void record_discontinuity();
    LatencyStats latency() const;       // last completed 1 s window
private:
    void roll_window_locked(int64_t now_us);
    mutable std::mutex                 m_;
    std::vector<std::shared_ptr<Sink>> sinks_;
    size_t                             default_depth_ = 4;
    mutable std::mutex lat_m_;
    LatencyStats last_;
    int64_t  win_start_us_ = 0; unsigned n_c2o_ = 0, n_o2s_ = 0; double sum_c2o_ = 0, max_c2o_ = 0, sum_o2s_ = 0, max_o2s_ = 0; unsigned disc_ = 0;
};

} // namespace machino
