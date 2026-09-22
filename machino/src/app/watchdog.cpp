#include "app/watchdog.hpp"
#include "core/log.hpp"

namespace machino {

static const char* MOD = "WDOG";

WatchdogService::WatchdogService(IWatchdogDevice& dev, int timeout_s, int feed_interval_ms)
    : dev_(dev), timeout_s_(timeout_s) {
    if (timeout_s_ < 1) timeout_s_ = 1;
    // A third of the timeout leaves room for two missed feeds before the
    // hardware fires, so one slow iteration is not a reset.
    feed_ms_ = feed_interval_ms > 0 ? feed_interval_ms : (timeout_s_ * 1000) / 3;
    if (feed_ms_ < 100) feed_ms_ = 100;
}

Result WatchdogService::start(int64_t now_ms) {
    std::lock_guard<std::mutex> lk(m_);
    if (open_) return Result::ok();
    if (!dev_.open()) {
        ++open_errors_;
        // Not fatal, and said once. A camera with no watchdog still has to run.
        LOGW(MOD, "watchdog unavailable - continuing without automatic recovery");
        return Result::unsupported();
    }
    open_ = true;
    ever_opened_ = true;
    effective_timeout_s_ = timeout_s_;
    int eff = timeout_s_;
    if (dev_.set_timeout(timeout_s_, eff)) {
        effective_timeout_s_ = eff;
        if (eff != timeout_s_)
            LOGW(MOD, "driver took %d s, not the %d s asked for", eff, timeout_s_);
    } else {
        LOGW(MOD, "WDIOC_SETTIMEOUT refused - using the driver's own timeout");
    }
    // Feed once immediately: the device is armed from the moment it opened, and
    // the first tick is a whole interval away.
    if (dev_.feed()) { ++feeds_; last_feed_ms_ = now_ms; }
    fed_at_epoch_ = epoch_;
    next_due_ms_  = now_ms + feed_ms_;
    LOGI(MOD, "armed: %s, timeout %d s, feeding every %d ms while the main loop advances",
         dev_.identity().c_str(), effective_timeout_s_, feed_ms_);
    return Result::ok();
}

void WatchdogService::stop() {
    std::lock_guard<std::mutex> lk(m_);
    if (!open_) return;
    open_ = false;
    // Ask for the magic close. On a driver built with nowayout this is refused
    // and the hardware will fire - which is why the disarm is requested rather
    // than assumed, and why the log says what was attempted.
    dev_.close(true);
    LOGI(MOD, "disarmed and closed after %llu feeds (%llu ticks skipped because the loop had not moved)",
         (unsigned long long)feeds_, (unsigned long long)skipped_);
}

bool WatchdogService::tick(int64_t now_ms) {
    std::lock_guard<std::mutex> lk(m_);
    if (!open_) return false;
    if (now_ms < next_due_ms_) return false;
    next_due_ms_ = now_ms + feed_ms_;

    // THE check. A feeder that skips this is a feeder that guarantees the
    // camera will not recover from a wedged main loop.
    if (epoch_ == fed_at_epoch_) {
        ++skipped_;
        // Deliberately not logged per tick: if the loop really is wedged the
        // log is not being written either, and if it is a one-off the line is
        // noise. The counter carries it into telemetry instead.
        return false;
    }
    fed_at_epoch_ = epoch_;
    if (!dev_.feed()) {
        ++feed_errors_;
        if (!warned_feed_) { warned_feed_ = true; LOGE(MOD, "feed failed - the hardware will fire unless this recovers"); }
        return false;
    }
    ++feeds_;
    last_feed_ms_ = now_ms;
    return true;
}

WatchdogStats WatchdogService::stats(int64_t now_ms) const {
    std::lock_guard<std::mutex> lk(m_);
    WatchdogStats s;
    s.available   = ever_opened_;
    s.enabled     = open_;
    s.timeout_s   = effective_timeout_s_;
    s.feeds       = feeds_;
    s.skipped     = skipped_;
    s.feed_errors = feed_errors_;
    s.open_errors = open_errors_;
    s.health_epoch = epoch_;
    s.last_feed_age_ms = last_feed_ms_ < 0 ? -1 : now_ms - last_feed_ms_;
    return s;
}

} // namespace machino
