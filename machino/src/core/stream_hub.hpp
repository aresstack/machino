// Machino core: fan-out of access units to N consumers with bounded,
// per-consumer queues. A slow consumer drops its own oldest frames; it never
// blocks the encoder thread or other consumers.
#pragma once
#include "core/frame.hpp"
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <vector>

namespace machino {

class Sink {
public:
    explicit Sink(size_t depth) : depth_(depth ? depth : 1) {}
    // Blocks up to timeout_ms; false on timeout or after close().
    bool pop(AuPtr& out, int timeout_ms);
    void push(const AuPtr& au);      // called by the hub
    void close();                    // wakes any waiter, pop() returns false
    bool closed() const { return closed_; }
    unsigned dropped() const { return dropped_; }
private:
    std::mutex              m_;
    std::condition_variable cv_;
    std::deque<AuPtr>       q_;
    size_t                  depth_;
    bool                    closed_  = false;
    unsigned                dropped_ = 0;
};

class StreamHub {
public:
    std::shared_ptr<Sink> subscribe(size_t depth = 8);
    void unsubscribe(const std::shared_ptr<Sink>& s);
    void publish(const AuPtr& au);
    size_t consumers() const;
private:
    mutable std::mutex                 m_;
    std::vector<std::shared_ptr<Sink>> sinks_;
};

} // namespace machino
