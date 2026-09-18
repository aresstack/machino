#include "core/stream_hub.hpp"
#include <algorithm>
#include <chrono>

namespace machino {

bool Sink::pop(AuPtr& out, int timeout_ms) {
    std::unique_lock<std::mutex> lk(m_);
    if (!cv_.wait_for(lk, std::chrono::milliseconds(timeout_ms),
                      [&] { return closed_ || !q_.empty(); })) return false;
    if (q_.empty()) return false;   // closed
    out = q_.front(); q_.pop_front();
    return true;
}

void Sink::push(const AuPtr& au) {
    {
        std::lock_guard<std::mutex> lk(m_);
        if (closed_) return;
        if (q_.size() >= depth_) {
            // Slow consumer: on a key frame flush everything older (a decoder
            // can resume from it), otherwise drop the oldest non-key frame.
            if (au->key) { dropped_ += (unsigned)q_.size(); q_.clear(); }
            else { q_.pop_front(); ++dropped_; }
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
    auto s = std::make_shared<Sink>(depth);
    std::lock_guard<std::mutex> lk(m_);
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

size_t StreamHub::consumers() const {
    std::lock_guard<std::mutex> lk(m_);
    return sinks_.size();
}

} // namespace machino
