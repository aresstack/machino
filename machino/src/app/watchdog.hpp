// AP4: hardware watchdog.
//
// Majestic armed this platform's watchdog and Machino silently dropped it, so
// every hang so far has needed a human with a power plug. The contract was read
// off the device rather than guessed:
//
//   /dev/watchdog          misc 10:130, driver ingenic,watchdog on 10002000.tcu
//   majestic's strings     open -> WDIOC_GETSUPPORT -> WDIOC_SETTIMEOUT -> feed
//   /etc/majestic.yaml     watchdog: enabled: true, timeout: 15
//   nowayout               NOT set - streamerctl records a hardware test where
//                          majestic released /dev/watchdog on stop and no reset
//                          followed, so closing really does disarm
//
// The device sits behind an interface for two reasons: the ioctl path needs
// POSIX and is not in the host test build, and every decision worth testing is
// in the policy rather than the syscall.
//
// THE POINT OF THE EPOCH: a watchdog fed by an independent timer thread proves
// only that the timer thread is alive. If the main loop is wedged while the
// feeder keeps running, the watchdog is worse than useless - it guarantees the
// camera will NOT recover. So the feeder never feeds on its own schedule; it
// feeds only when the main loop has advanced since the last feed.
#pragma once
#include "core/result.hpp"
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

namespace machino {

// The kernel side, injectable. Every method reports success rather than
// throwing; a camera must start even when its watchdog will not.
class IWatchdogDevice {
public:
    virtual ~IWatchdogDevice() = default;
    // Opens and thereby ARMS the watchdog. false = unavailable.
    virtual bool open() = 0;
    // WDIOC_SETTIMEOUT. `effective` receives what the driver actually took,
    // which can differ from the request.
    virtual bool set_timeout(int seconds, int& effective) = 0;
    virtual bool feed() = 0;
    // `disarm` asks for the magic close ('V'), which only works when the
    // driver was built without nowayout.
    virtual void close(bool disarm) = 0;
    // Free-form identity for the log, e.g. from WDIOC_GETSUPPORT.
    virtual std::string identity() const = 0;
};

struct WatchdogStats {
    bool     available = false;      // the device opened
    bool     enabled = false;        // configured on AND open
    int      timeout_s = 0;          // what the driver took
    uint64_t feeds = 0;
    uint64_t skipped = 0;            // ticks that did NOT feed: the loop had not moved
    uint64_t feed_errors = 0;
    uint64_t open_errors = 0;
    uint64_t health_epoch = 0;
    int64_t  last_feed_age_ms = -1;  // -1 = never fed
};

class WatchdogService {
public:
    // `feed_interval_ms` of 0 derives one third of the timeout, which leaves
    // two missed feeds of headroom before the hardware fires.
    WatchdogService(IWatchdogDevice& dev, int timeout_s, int feed_interval_ms = 0);

    // Opens and configures. Returns error when the device is unavailable - the
    // caller logs it and carries on; a missing watchdog must never stop a
    // camera from streaming.
    Result start(int64_t now_ms);
    // Disarm and close. Safe to call twice.
    void   stop();

    // Called by the MAIN LOOP on every iteration. This is the only thing that
    // makes a feed legitimate.
    void   heartbeat() { std::lock_guard<std::mutex> lk(m_); ++epoch_; }

    // Called by the feeder on its own schedule. Feeds only when the main loop
    // has moved since the last feed. Returns true when it fed.
    bool   tick(int64_t now_ms);

    // Milliseconds until the next tick is due, for a bounded sleep.
    int    feed_interval_ms() const { return feed_ms_; }

    WatchdogStats stats(int64_t now_ms) const;

private:
    IWatchdogDevice&  dev_;
    int               timeout_s_;
    int               feed_ms_;
    mutable std::mutex m_;
    // Plain integers under a mutex, not std::atomic<uint64_t>: MIPS32 has no
    // lock-free 64-bit atomics and they fail the cross link.
    uint64_t          epoch_ = 0;
    uint64_t          fed_at_epoch_ = 0;
    uint64_t          feeds_ = 0, skipped_ = 0, feed_errors_ = 0, open_errors_ = 0;
    int64_t           next_due_ms_ = 0;
    int64_t           last_feed_ms_ = -1;
    bool              open_ = false;
    bool              ever_opened_ = false;   // the device DID open at least once
    int               effective_timeout_s_ = 0;
    bool              warned_feed_ = false;    // one error line, never a spam loop
};

} // namespace machino
