// Lifecycle tests: PipelineManager state machine with a fake platform and a
// fake grace timer. Included from test_main.cpp's binary.
#include "core/lifecycle/pipeline_manager.hpp"
#include "core/stream_hub.hpp"
#include "fake_platform.hpp"

#include <atomic>
#include <cstdio>
#include <thread>
#include <vector>

using namespace machino;
using namespace machino::lifecycle;
using namespace machino::test;

extern int g_fail_ext, g_pass_ext;
#define LCHECK(cond) do { if (cond) { ++g_pass_ext; } else { ++g_fail_ext; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

namespace {

struct Rig {
    CallLog       log;
    FakePlatform  platform{log};
    FakeTimer     timer;
    StreamHub     hub;
    EffectiveStream stream{};
    LifecycleConfig cfg{};
    PipelineManager mgr;
    Rig(int grace = 5000) : mgr(platform, stream, mk(grace), timer, hub) {}
    static LifecycleConfig mk(int g) { LifecycleConfig c; c.idle_grace_ms = g; c.poll_timeout_ms = 10; return c; }
};

void test_first_acquire_starts_once() {
    Rig r;
    LCHECK(r.mgr.state() == State::ColdIdle);
    auto d = r.mgr.acquire(ConsumerType::Rtsp);
    LCHECK(d.active() && r.mgr.state() == State::Active);
    LCHECK(r.log.count("platform.bring_up") == 1 && r.log.count("enc.start") == 1);
    Stats s = r.mgr.stats();
    LCHECK(s.generation == 1 && s.start_count == 1 && s.total_demand == 1 && s.demand[(int)ConsumerType::Rtsp] == 1);
    // exact bring-up order
    auto c = r.log.snapshot();
    std::vector<std::string> want = {"platform.bring_up", "fs.create", "enc.create", "bind", "fs.enable", "enc.start"};
    LCHECK(c.size() >= want.size());
    bool ok = c.size() >= want.size();
    for (size_t i = 0; ok && i < want.size(); ++i) ok = (c[i] == want[i]);
    LCHECK(ok);
}

void test_second_acquire_no_second_start() {
    Rig r;
    auto a = r.mgr.acquire(ConsumerType::Rtsp);
    auto b = r.mgr.acquire(ConsumerType::Recording);
    LCHECK(r.log.count("platform.bring_up") == 1);
    Stats s = r.mgr.stats();
    LCHECK(s.total_demand == 2 && s.demand[(int)ConsumerType::Recording] == 1 && s.generation == 1);
}

void test_release_one_of_two_keeps_active() {
    Rig r;
    auto a = r.mgr.acquire(ConsumerType::Rtsp);
    { auto b = r.mgr.acquire(ConsumerType::Rtsp); }
    LCHECK(r.mgr.state() == State::Active && !r.timer.armed);
    LCHECK(r.mgr.stats().total_demand == 1);
}

void test_final_release_enters_grace() {
    Rig r(4321);
    { auto a = r.mgr.acquire(ConsumerType::Rtsp); }
    LCHECK(r.mgr.state() == State::GraceIdle);
    LCHECK(r.timer.armed && r.timer.last_ms == 4321);
    LCHECK(r.log.count("platform.tear_down") == 0);   // nothing torn down yet
}

void test_acquire_during_grace_cancels_shutdown() {
    Rig r;
    { auto a = r.mgr.acquire(ConsumerType::Rtsp); }
    LCHECK(r.mgr.state() == State::GraceIdle);
    auto b = r.mgr.acquire(ConsumerType::Rtsp);
    LCHECK(r.mgr.state() == State::Active && !r.timer.armed && r.timer.disarm_count >= 1);
    LCHECK(r.log.count("platform.bring_up") == 1);    // same generation continues
    LCHECK(r.mgr.stats().generation == 1);
}

void test_grace_expiry_stops_once() {
    Rig r;
    { auto a = r.mgr.acquire(ConsumerType::Rtsp); }
    r.mgr.on_grace_timeout();
    LCHECK(r.mgr.state() == State::ColdIdle);
    LCHECK(r.log.count("platform.tear_down") == 1 && r.log.count("enc.stop") == 1);
    r.mgr.on_grace_timeout();                         // stale/duplicate expiry
    LCHECK(r.log.count("platform.tear_down") == 1 && r.mgr.stats().stop_count == 1);
    // exact cold teardown order
    auto c = r.log.snapshot();
    std::vector<std::string> want = {"enc.stop", "fs.disable", "unbind", "enc.destroy", "fs.destroy", "platform.tear_down"};
    size_t k = 0;
    for (auto& x : c) if (k < want.size() && x == want[k]) ++k;
    LCHECK(k == want.size());
}

void test_restart_from_cold() {
    Rig r;
    for (int i = 1; i <= 3; ++i) {
        { auto a = r.mgr.acquire(ConsumerType::Rtsp); LCHECK(a.active()); }
        r.mgr.on_grace_timeout();
        LCHECK(r.mgr.state() == State::ColdIdle);
        LCHECK(r.log.count("platform.bring_up") == i && r.log.count("platform.tear_down") == i);
    }
    LCHECK(r.mgr.stats().generation == 3 && r.mgr.stats().stop_count == 3);
}

void test_raii_release() {
    Rig r;
    {
        DemandHandle d = r.mgr.acquire(ConsumerType::Snapshot);
        LCHECK(r.mgr.stats().demand[(int)ConsumerType::Snapshot] == 1);
        DemandHandle moved = std::move(d);            // move keeps exactly one demand
        LCHECK(!d.active() && moved.active() && r.mgr.stats().total_demand == 1);
    }
    LCHECK(r.mgr.stats().total_demand == 0 && r.mgr.state() == State::GraceIdle);
    DemandHandle e;                                   // empty handle: release is a no-op
    e.release();
    LCHECK(r.mgr.stats().total_demand == 0);
}

void test_failed_start_rolls_back() {
    Rig r;
    r.platform.fail_at = FakePlatform::FailAt::Bind; r.platform.fail_times = 1;
    Result res; auto d = r.mgr.acquire(ConsumerType::Rtsp, &res);
    LCHECK(!d.active() && !res && r.mgr.state() == State::Failed);
    LCHECK(r.log.count("enc.destroy") == 1 && r.log.count("fs.destroy") == 1 && r.log.count("platform.tear_down") == 1);
    LCHECK(!r.platform.is_up());
    Stats s = r.mgr.stats();
    LCHECK(s.failed_count == 1 && s.generation == 0 && s.total_demand == 0 && s.last_error.find("bind") != std::string::npos);
    // a later consumer may try again (no automatic retry loop happened in between)
    LCHECK(r.log.count("platform.bring_up") == 1);
    auto e = r.mgr.acquire(ConsumerType::Rtsp);
    LCHECK(e.active() && r.mgr.state() == State::Active && r.mgr.stats().generation == 1);
    // failure at encoder start
    Rig q; q.platform.fail_at = FakePlatform::FailAt::EncoderStart; q.platform.fail_times = 1;
    auto f = q.mgr.acquire(ConsumerType::Rtsp);
    LCHECK(!f.active() && q.mgr.state() == State::Failed && q.log.count("platform.tear_down") == 1 && !q.platform.is_up());
}

void test_concurrent_acquire_release() {
    Rig r;
    std::atomic<int> active{0}, max_active{0};
    std::vector<std::thread> ts;
    for (int t = 0; t < 8; ++t) ts.emplace_back([&] {
        for (int i = 0; i < 200; ++i) {
            auto d = r.mgr.acquire(ConsumerType::Rtsp);
            int a = ++active; int m = max_active.load(); while (a > m && !max_active.compare_exchange_weak(m, a)) {}
            if ((i & 7) == 0) r.mgr.on_grace_timeout();  // stale expiries interleaved
            --active;
        }
    });
    for (auto& t : ts) t.join();
    Stats s = r.mgr.stats();
    LCHECK(s.total_demand == 0 && s.demand[(int)ConsumerType::Rtsp] == 0);
    LCHECK(s.state == State::GraceIdle || s.state == State::ColdIdle || s.state == State::Active);
    LCHECK(r.log.count("platform.bring_up") == r.log.count("platform.tear_down") + (r.platform.is_up() ? 1 : 0));
    r.mgr.on_grace_timeout();
    LCHECK(r.mgr.state() == State::ColdIdle);
    LCHECK(r.log.count("platform.bring_up") == r.log.count("platform.tear_down"));
    LCHECK(max_active.load() >= 2);                   // demand really overlapped
}

void test_timer_fires_while_demand_arrives() {
    Rig r;
    { auto a = r.mgr.acquire(ConsumerType::Rtsp); }
    LCHECK(r.mgr.state() == State::GraceIdle);
    // demand returns first, then the (already queued) expiry is delivered
    auto b = r.mgr.acquire(ConsumerType::Rtsp);
    r.mgr.on_grace_timeout();
    LCHECK(r.mgr.state() == State::Active && r.log.count("platform.tear_down") == 0);
    // expiry and a new acquire racing on threads: never a torn-down ACTIVE pipeline
    for (int i = 0; i < 50; ++i) {
        b.release();
        std::thread t1([&] { r.mgr.on_grace_timeout(); });
        std::thread t2([&] { b = r.mgr.acquire(ConsumerType::Rtsp); });
        t1.join(); t2.join();
        LCHECK(b.active() && r.mgr.state() == State::Active);
        LCHECK(r.log.count("platform.bring_up") == r.log.count("platform.tear_down") + 1);
    }
}

void test_shutdown_with_outstanding_demand() {
    Rig r;
    auto a = r.mgr.acquire(ConsumerType::Rtsp);
    r.mgr.shutdown();
    LCHECK(r.mgr.state() == State::ColdIdle && r.log.count("platform.tear_down") == 1);
    auto b = r.mgr.acquire(ConsumerType::Rtsp);       // refused after shutdown
    LCHECK(!b.active() && r.log.count("platform.bring_up") == 1);
}

} // namespace

void run_lifecycle_tests() {
    test_first_acquire_starts_once();
    test_second_acquire_no_second_start();
    test_release_one_of_two_keeps_active();
    test_final_release_enters_grace();
    test_acquire_during_grace_cancels_shutdown();
    test_grace_expiry_stops_once();
    test_restart_from_cold();
    test_raii_release();
    test_failed_start_rolls_back();
    test_concurrent_acquire_release();
    test_timer_fires_while_demand_arrives();
    test_shutdown_with_outstanding_demand();
}
