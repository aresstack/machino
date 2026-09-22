// AP4.10: the watchdog policy. The syscalls are behind IWatchdogDevice, so
// everything worth getting wrong is testable here.
//
// The test that matters most is the LAST one: a feeder that keeps feeding while
// the main loop is wedged does not merely fail to help, it guarantees the
// camera will never recover. Every other assertion is about not making the
// camera reset for a bad reason.
#include "app/watchdog.hpp"
#include <cstdio>
#include <string>
#include <vector>

using namespace machino;

extern int g_fail_ext, g_pass_ext;
#define WDCHECK(cond) do { if (cond) { ++g_pass_ext; } else { ++g_fail_ext; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

namespace {

struct FakeWatchdog : IWatchdogDevice {
    bool can_open = true;
    bool settimeout_ok = true;
    int  driver_timeout = 0;             // 0 = take what is asked for
    bool feed_ok = true;

    int  opens = 0, feeds = 0, closes = 0, settimeouts = 0;
    bool disarm_requested = false;
    bool is_open = false;

    bool open() override { ++opens; if (!can_open) return false; is_open = true; return true; }
    bool set_timeout(int s, int& effective) override {
        ++settimeouts;
        if (!settimeout_ok) return false;
        effective = driver_timeout > 0 ? driver_timeout : s;
        return true;
    }
    bool feed() override { ++feeds; return feed_ok; }
    void close(bool disarm) override { ++closes; disarm_requested = disarm; is_open = false; }
    std::string identity() const override { return "fake-wdt"; }
};

} // namespace

void run_watchdog_tests() {
    // --- a healthy main loop is fed ----------------------------------------
    {
        FakeWatchdog d;
        WatchdogService w(d, 15, 1000);
        WDCHECK(w.start(0));
        WDCHECK(d.opens == 1 && d.is_open);
        WDCHECK(d.feeds == 1);                       // armed at open, so fed at once
        WDCHECK(w.stats(0).timeout_s == 15);
        WDCHECK(w.stats(0).enabled && w.stats(0).available);

        // the loop runs, the feeder ticks
        for (int t = 1000; t <= 5000; t += 1000) {
            w.heartbeat();
            WDCHECK(w.tick(t));
        }
        WatchdogStats s = w.stats(5000);
        WDCHECK(s.feeds == 6);                       // 1 at start + 5 ticks
        WDCHECK(s.skipped == 0);
        WDCHECK(s.last_feed_age_ms == 0);
        WDCHECK(s.health_epoch == 5);
    }

    // --- THE ONE THAT MATTERS: feeder alive, main loop frozen -> NO feed ----
    {
        FakeWatchdog d;
        WatchdogService w(d, 15, 1000);
        w.start(0);
        const int fed_at_start = d.feeds;
        // the feeder keeps ticking on schedule; the loop never advances
        for (int t = 1000; t <= 20000; t += 1000) WDCHECK(!w.tick(t));
        WDCHECK(d.feeds == fed_at_start);            // not one more feed
        WatchdogStats s = w.stats(20000);
        WDCHECK(s.skipped == 20);
        WDCHECK(s.last_feed_age_ms == 20000);        // the hardware would have fired
    }

    // --- one frozen interval then recovery: feeds resume -------------------
    {
        FakeWatchdog d;
        WatchdogService w(d, 15, 1000);
        w.start(0);
        WDCHECK(!w.tick(1000));                      // loop stalled for one interval
        w.heartbeat();
        WDCHECK(w.tick(2000));                       // and came back
        WDCHECK(w.stats(2000).skipped == 1);
    }

    // --- many heartbeats between ticks still feed exactly once -------------
    {
        FakeWatchdog d;
        WatchdogService w(d, 15, 1000);
        w.start(0);
        const int before = d.feeds;
        for (int i = 0; i < 50; ++i) w.heartbeat();
        WDCHECK(w.tick(1000));
        WDCHECK(d.feeds == before + 1);              // a feed per tick, not per heartbeat
    }

    // --- ticking before the interval is due does nothing -------------------
    {
        FakeWatchdog d;
        WatchdogService w(d, 15, 1000);
        w.start(0);
        w.heartbeat();
        WDCHECK(!w.tick(500));                       // too early
        WDCHECK(w.stats(500).skipped == 0);          // and not counted as a stall
        WDCHECK(w.tick(1000));
    }

    // --- COLD_IDLE is healthy: nothing here is coupled to media ------------
    // The service never sees frames, sessions or encoders; only the loop.
    {
        FakeWatchdog d;
        WatchdogService w(d, 15, 1000);
        w.start(0);
        for (int t = 1000; t <= 60000; t += 1000) { w.heartbeat(); WDCHECK(w.tick(t)); }
        WDCHECK(w.stats(60000).skipped == 0);
    }

    // --- an unavailable device must not stop the daemon --------------------
    {
        FakeWatchdog d; d.can_open = false;
        WatchdogService w(d, 15, 1000);
        Result r = w.start(0);
        WDCHECK(!r);
        WDCHECK(r.status == Status::Unsupported);
        WatchdogStats s = w.stats(0);
        WDCHECK(!s.available && !s.enabled);
        WDCHECK(s.open_errors == 1);
        // and it stays quiet rather than retrying forever
        w.heartbeat();
        WDCHECK(!w.tick(1000));
        WDCHECK(d.opens == 1);
        w.stop();                                    // safe with nothing open
        WDCHECK(d.closes == 0);
    }

    // --- SETTIMEOUT refused: still armed, with the driver's own timeout ----
    {
        FakeWatchdog d; d.settimeout_ok = false;
        WatchdogService w(d, 15, 1000);
        WDCHECK(w.start(0));
        WDCHECK(w.stats(0).enabled);
        w.heartbeat();
        WDCHECK(w.tick(1000));                       // feeding regardless
    }
    // --- the driver takes a DIFFERENT timeout than asked -------------------
    {
        FakeWatchdog d; d.driver_timeout = 30;
        WatchdogService w(d, 15, 1000);
        w.start(0);
        WDCHECK(w.stats(0).timeout_s == 30);         // reported as taken, not as asked
    }

    // --- a failing feed is recorded, never a crash, never a log storm ------
    {
        FakeWatchdog d; d.feed_ok = false;
        WatchdogService w(d, 15, 1000);
        w.start(0);
        for (int t = 1000; t <= 10000; t += 1000) { w.heartbeat(); WDCHECK(!w.tick(t)); }
        WatchdogStats s = w.stats(10000);
        WDCHECK(s.feed_errors == 10);
        WDCHECK(s.feeds == 0);                       // the start feed failed too
    }

    // --- shutdown disarms, and is idempotent -------------------------------
    {
        FakeWatchdog d;
        WatchdogService w(d, 15, 1000);
        w.start(0);
        w.stop();
        WDCHECK(d.closes == 1);
        WDCHECK(d.disarm_requested);                 // magic close ASKED for
        WDCHECK(!d.is_open);
        WDCHECK(!w.stats(0).enabled);
        w.stop();
        WDCHECK(d.closes == 1);                      // not closed twice
        // and it does not feed a closed device
        w.heartbeat();
        WDCHECK(!w.tick(5000));
    }

    // --- start() twice does not re-arm or double-feed ----------------------
    {
        FakeWatchdog d;
        WatchdogService w(d, 15, 1000);
        w.start(0);
        const int opens = d.opens, feeds = d.feeds;
        WDCHECK(w.start(100));
        WDCHECK(d.opens == opens && d.feeds == feeds);
    }

    // --- the derived feed interval leaves room for two missed feeds --------
    {
        FakeWatchdog d;
        WatchdogService w(d, 15);                    // no explicit interval
        WDCHECK(w.feed_interval_ms() == 5000);       // 15 s / 3
        FakeWatchdog d2;
        WatchdogService w2(d2, 1);
        WDCHECK(w2.feed_interval_ms() >= 100);       // never degenerate
        FakeWatchdog d3;
        WatchdogService w3(d3, 0);                   // nonsense timeout is clamped
        WDCHECK(w3.feed_interval_ms() >= 100);
    }
}
