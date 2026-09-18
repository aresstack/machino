#include "core/stream_hub.hpp"
#include <algorithm>
#include <chrono>
#include <ctime>

namespace machino {

static int64_t mono_us() { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000; }

bool Sink::pop(AuPtr& out, int timeout_ms, bool* discontinuity) {
    std::unique_lock<std::mutex> lk(m_);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    for (;;) {
        if (!cv_.wait_until(lk, deadline, [&] { return closed_ || !q_.empty(); })) return false;
        if (q_.empty()) return false;
        out = q_.front(); q_.pop_front();
        if (!disc_) {
            if (discontinuity) *discontinuity = false;
            return true;
        }
        // At least one reference frame was lost. P-frames that still happen
        // to be queued are unusable, so discard them instead of feeding a
        // decoder a corrupt dependency chain. The next IDR is the first
        // useful frame and explicitly marks the discontinuity to the sink.
        if (out->key) {
            disc_ = false;
            if (discontinuity) *discontinuity = true;
            return true;
        }
        ++dropped_;
    }
}

void Sink::push(const AuPtr& au) {
    {
        std::lock_guard<std::mutex> lk(m_);
        if (closed_) return;
        if (q_.size() >= depth_) {
            // stale-frame policy: never grow, drop the oldest; a key frame
            // supersedes everything queued before it
            if (au->key) { dropped_ += (unsigned)q_.size(); q_.clear(); }
            else { q_.pop_front(); ++dropped_; disc_ = true; }
        }
        q_.push_back(au);
    }
    cv_.notify_one();
}

void Sink::close() {
    { std::lock_guard<std::mutex> lk(m_); closed_ = true; q_.clear(); }
    cv_.notify_all();
}

std::shared_ptr<Sink> StreamHub::subscribe(size_t depth) {
    std::lock_guard<std::mutex> lk(m_);
    auto s = std::make_shared<Sink>(depth ? depth : default_depth_);
    sinks_.push_back(s);
    return s;
}

void StreamHub::unsubscribe(const std::shared_ptr<Sink>& s) {
    if (!s) return;
    s->close();
    std::lock_guard<std::mutex> lk(m_);
    sinks_.erase(std::remove(sinks_.begin(), sinks_.end(), s), sinks_.end());
}

void StreamHub::publish(const AuPtr& au) {
    std::lock_guard<std::mutex> lk(m_);
    for (auto& s : sinks_) s->push(au);
}

size_t StreamHub::consumers() const { std::lock_guard<std::mutex> lk(m_); return sinks_.size(); }

void StreamHub::roll_window_locked(int64_t now) {
    if (win_start_us_ == 0) { win_start_us_ = now; return; }
    if (now - win_start_us_ < 1000000) return;
    LatencyStats s; s.valid = (n_c2o_ + n_o2s_) > 0; s.samples = n_c2o_;
    if (n_c2o_) { s.capture_to_out_avg_ms = sum_c2o_ / n_c2o_ / 1000.0; s.capture_to_out_max_ms = max_c2o_ / 1000.0; }
    if (n_o2s_) { s.out_to_send_avg_ms = sum_o2s_ / n_o2s_ / 1000.0; s.out_to_send_max_ms = max_o2s_ / 1000.0; }
    s.discontinuities = disc_;
    last_ = s;
    win_start_us_ = now; n_c2o_ = n_o2s_ = 0; sum_c2o_ = max_c2o_ = sum_o2s_ = max_o2s_ = 0; disc_ = 0;
}

void StreamHub::record_capture_to_out(int64_t us) {
    std::lock_guard<std::mutex> lk(lat_m_);
    if (us >= 0 && us < 10000000) { ++n_c2o_; sum_c2o_ += (double)us; if (us > max_c2o_) max_c2o_ = (double)us; }
    roll_window_locked(mono_us());
}

void StreamHub::record_out_to_send(int64_t us) {
    std::lock_guard<std::mutex> lk(lat_m_);
    if (us >= 0 && us < 10000000) { ++n_o2s_; sum_o2s_ += (double)us; if (us > max_o2s_) max_o2s_ = (double)us; }
    roll_window_locked(mono_us());
}

void StreamHub::record_discontinuity() { std::lock_guard<std::mutex> lk(lat_m_); ++disc_; }

LatencyStats StreamHub::latency() const { std::lock_guard<std::mutex> lk(lat_m_); return last_; }

} // namespace machino
