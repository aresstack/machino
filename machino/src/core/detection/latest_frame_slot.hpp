// Machino core: bounded single-slot hand-off for frame-driven detectors.
// One in-flight inference, one replaceable latest candidate, never a FIFO
// backlog (GPT M9 preflight). A producer offers frames; when the detector is
// busy the newest frame replaces the pending one and the drop is counted, so
// inference always runs on fresh data and never falls behind real time.
#pragma once
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <vector>

namespace machino { namespace detection {

class LatestFrameSlot {
public:
    // Replace the pending frame with this one. Returns false (and counts a
    // skip) when a frame was already waiting - it is overwritten, not queued.
    bool offer(const uint8_t* data, size_t len, int w, int h, int stride, int64_t pts_us) {
        std::lock_guard<std::mutex> lk(m_);
        if (closed_) return false;
        bool replaced = have_;
        buf_.assign(data, data + len);
        w_ = w; h_ = h; stride_ = stride; pts_ = pts_us; have_ = true;
        if (replaced) ++skipped_;
        cv_.notify_one();
        return !replaced;
    }

    // Take the pending frame into `out` (kept capacity). false on timeout/close.
    bool take(std::vector<uint8_t>& out, int& w, int& h, int& stride, int64_t& pts, int timeout_ms) {
        std::unique_lock<std::mutex> lk(m_);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        if (!cv_.wait_until(lk, deadline, [&] { return closed_ || have_; })) return false;
        if (closed_ || !have_) return false;
        out.swap(buf_); w = w_; h = h_; stride = stride_; pts = pts_; have_ = false;
        return true;
    }

    void close() { std::lock_guard<std::mutex> lk(m_); closed_ = true; cv_.notify_all(); }
    unsigned skipped() const { std::lock_guard<std::mutex> lk(m_); return skipped_; }

private:
    mutable std::mutex      m_;
    std::condition_variable cv_;
    std::vector<uint8_t>    buf_;
    int      w_ = 0, h_ = 0, stride_ = 0;
    int64_t  pts_ = 0;
    bool     have_ = false, closed_ = false;
    unsigned skipped_ = 0;
};

}} // namespace machino::detection
