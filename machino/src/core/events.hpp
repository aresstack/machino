// Machino core: event bus for the control side (lifecycle, config_changed,
// telemetry, error). Publishers never block: each subscriber owns a bounded
// queue; when it overflows the oldest event is dropped and the subscription
// is flagged so the transport can disconnect the slow client.
#pragma once
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace machino {

struct Event {
    std::string type;    // "lifecycle" | "config_changed" | "telemetry" | "error"
    std::string data;    // JSON text
};

class Subscription {
public:
    explicit Subscription(size_t depth) : depth_(depth ? depth : 1) {}
    bool pop(Event& out);                 // non-blocking
    bool overflowed() const { std::lock_guard<std::mutex> lk(m_); return overflowed_; }
    size_t pending() const { std::lock_guard<std::mutex> lk(m_); return q_.size(); }
private:
    friend class EventBus;
    void push(const Event& e);
    mutable std::mutex m_;
    std::deque<Event> q_;
    size_t depth_;
    bool   overflowed_ = false;
};

class EventBus {
public:
    std::shared_ptr<Subscription> subscribe(size_t depth = 64);
    void unsubscribe(const std::shared_ptr<Subscription>& s);
    void publish(const std::string& type, const std::string& json);
    size_t subscribers() const;
private:
    mutable std::mutex m_;
    std::vector<std::shared_ptr<Subscription>> subs_;
};

} // namespace machino
