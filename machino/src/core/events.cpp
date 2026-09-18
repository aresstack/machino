#include "core/events.hpp"
#include <algorithm>

namespace machino {

bool Subscription::pop(Event& out) {
    std::lock_guard<std::mutex> lk(m_);
    if (q_.empty()) return false;
    out = q_.front(); q_.pop_front();
    return true;
}

void Subscription::push(const Event& e) {
    std::lock_guard<std::mutex> lk(m_);
    if (q_.size() >= depth_) { q_.pop_front(); overflowed_ = true; }
    q_.push_back(e);
}

std::shared_ptr<Subscription> EventBus::subscribe(size_t depth) {
    auto s = std::make_shared<Subscription>(depth);
    std::lock_guard<std::mutex> lk(m_);
    subs_.push_back(s);
    return s;
}

void EventBus::unsubscribe(const std::shared_ptr<Subscription>& s) {
    std::lock_guard<std::mutex> lk(m_);
    subs_.erase(std::remove(subs_.begin(), subs_.end(), s), subs_.end());
}

void EventBus::publish(const std::string& type, const std::string& json) {
    Event e{type, json};
    std::lock_guard<std::mutex> lk(m_);
    for (auto& s : subs_) s->push(e);
}

size_t EventBus::subscribers() const { std::lock_guard<std::mutex> lk(m_); return subs_.size(); }

} // namespace machino
