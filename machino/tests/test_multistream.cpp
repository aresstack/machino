// M8: multi-stream lifecycle. What matters here is ownership: one base
// (sensor/ISP) shared by independent units (main H.264, sub H.264, JPEG),
// where a unit exists exactly while someone needs it - the spec's case matrix
// "only ch0 / only ch1 / ch0+ch1 / snapshot only / ch0+snapshot / no consumers".
#include "core/hw/registry.hpp"
#include "core/hw/resolve.hpp"
#include "core/lifecycle/pipeline_manager.hpp"
#include "core/stream_hub.hpp"
#include "fake_platform.hpp"

#include <cstdio>

using namespace machino;
using namespace machino::lifecycle;
using namespace machino::test;

extern int g_fail_ext, g_pass_ext;
#define MCHECK(cond) do { if (cond) { ++g_pass_ext; } else { ++g_fail_ext; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

namespace {

struct Rig {
    CallLog log;
    FakePlatform platform{log};
    FakeTimer base_timer, sub_timer, jpeg_timer, main_timer;
    StreamHub hub, sub_hub;
    EffectiveStream main_s, sub_s;
    LifecycleConfig lc;
    PipelineManager mgr;

    static EffectiveStream mk(int w, int h, int fps, int kbps) {
        EffectiveStream s; s.width = w; s.height = h; s.fps = fps; s.bitrate_kbps = kbps;
        s.native_width = 1920; s.native_height = 1080; return s;
    }
    static LifecycleConfig mk_lc() { LifecycleConfig c; c.idle_grace_ms = 1000; c.poll_timeout_ms = 10; return c; }

    explicit Rig(bool with_sub = true, bool with_jpeg = true)
        : main_s(mk(1920, 1080, 20, 3000)), sub_s(mk(640, 360, 10, 512)), lc(mk_lc()),
          mgr(platform, main_s, lc, base_timer, hub) {
        mgr.set_unit_grace_timer(UNIT_MAIN, &main_timer);
        if (with_sub) mgr.configure_sub(sub_s, sub_hub, &sub_timer);
        if (with_jpeg) { JpegParams p; p.quality = 80; mgr.configure_jpeg(p, 300, 2000, &jpeg_timer); }
    }
};

// ---- only ch1: the substream alone brings the base up, main stays off ------
void test_sub_only() {
    Rig r;
    auto d = r.mgr.acquire_unit(UNIT_SUB, ConsumerType::Rtsp);
    MCHECK(d.active() && r.mgr.state() == State::Active);
    MCHECK(r.mgr.unit_active(UNIT_SUB) && !r.mgr.unit_active(UNIT_MAIN));
    MCHECK(r.platform.last_stream.width == 640);           // the sub geometry reached the adapter
    MCHECK(r.log.count("platform.bring_up") == 1 && r.log.count("enc.create") == 1);
    d.release();
    MCHECK(r.mgr.state() == State::GraceIdle && r.base_timer.armed);
    r.mgr.on_grace_timeout();
    MCHECK(r.mgr.state() == State::ColdIdle);
    MCHECK(r.log.count("platform.tear_down") == 1 && r.log.count("enc.destroy") == 1);
}

// ---- ch0 + ch1: independent demand, sub winds down on its own grace --------
void test_main_plus_sub() {
    Rig r;
    auto m = r.mgr.acquire(ConsumerType::Rtsp);
    auto s = r.mgr.acquire_unit(UNIT_SUB, ConsumerType::Rtsp);
    MCHECK(m.active() && s.active());
    MCHECK(r.mgr.unit_active(UNIT_MAIN) && r.mgr.unit_active(UNIT_SUB));
    MCHECK(r.log.count("platform.bring_up") == 1);          // ONE base for both
    MCHECK(r.log.count("enc.create") == 2);

    s.release();                                            // sub leaves, main stays
    MCHECK(r.mgr.state() == State::Active);                  // base unaffected
    MCHECK(r.sub_timer.armed && r.mgr.unit_active(UNIT_SUB)); // warm until its grace
    r.mgr.on_unit_grace(UNIT_SUB);
    MCHECK(!r.mgr.unit_active(UNIT_SUB) && r.mgr.unit_active(UNIT_MAIN));
    MCHECK(r.mgr.state() == State::Active);
    MCHECK(r.log.count("platform.tear_down") == 0);          // the base never blinked

    // sub demand returns -> only the sub unit starts again
    auto s2 = r.mgr.acquire_unit(UNIT_SUB, ConsumerType::Rtsp);
    MCHECK(s2.active() && r.mgr.unit_active(UNIT_SUB));
    MCHECK(r.log.count("platform.bring_up") == 1);
}

// ---- last consumer leaves: everything through base grace, units stay warm --
void test_base_grace_keeps_units_warm() {
    Rig r;
    auto m = r.mgr.acquire(ConsumerType::Rtsp);
    auto s = r.mgr.acquire_unit(UNIT_SUB, ConsumerType::Rtsp);
    s.release();
    m.release();
    MCHECK(r.mgr.state() == State::GraceIdle);
    MCHECK(!r.sub_timer.armed);                              // base grace supersedes unit grace
    MCHECK(r.mgr.unit_active(UNIT_MAIN) && r.mgr.unit_active(UNIT_SUB));
    auto m2 = r.mgr.acquire(ConsumerType::Rtsp);             // fast rejoin
    MCHECK(r.mgr.state() == State::Active && r.log.count("platform.bring_up") == 1);
    m2.release();
    r.mgr.on_grace_timeout();
    MCHECK(r.mgr.state() == State::ColdIdle && r.log.count("enc.destroy") == 2);
}

// ---- snapshot from cold: wake, capture, back to cold via grace --------------
void test_snapshot_from_cold() {
    Rig r;
    std::vector<uint8_t> jpg; std::string err;
    MCHECK(r.mgr.state() == State::ColdIdle);
    Result res = r.mgr.snapshot(jpg, err);
    MCHECK(res && jpg.size() >= 4);
    MCHECK(jpg[0] == 0xFF && jpg[1] == 0xD8);                // SOI
    MCHECK(jpg[jpg.size() - 2] == 0xFF && jpg[jpg.size() - 1] == 0xD9);  // EOI
    MCHECK(r.log.count("platform.bring_up") == 1);
    MCHECK(r.log.count("enc.create") == 0);                  // no H.264 encoder for a snapshot
    MCHECK(r.mgr.state() == State::GraceIdle);               // demand ended with the request
    r.mgr.on_grace_timeout();
    MCHECK(r.mgr.state() == State::ColdIdle);
    MCHECK(r.log.count("jpeg.destroy") == 1);                // nothing stays behind
    MCHECK(r.mgr.stats().jpeg_captures == 1);
}

// ---- snapshot during main: reuses the base, never restarts the stream ------
void test_snapshot_during_main() {
    Rig r;
    auto m = r.mgr.acquire(ConsumerType::Rtsp);
    unsigned gen = r.mgr.stats().generation;
    std::vector<uint8_t> jpg; std::string err;
    MCHECK(r.mgr.snapshot(jpg, err));
    MCHECK(r.mgr.stats().generation == gen);                 // no restart
    MCHECK(r.log.count("platform.bring_up") == 1);
    MCHECK(r.mgr.state() == State::Active);                  // main demand still holds the base
    MCHECK(r.jpeg_timer.armed);                              // jpeg warm for its grace
    r.mgr.on_jpeg_grace();
    MCHECK(r.log.count("jpeg.destroy") == 1);
    MCHECK(r.mgr.unit_active(UNIT_MAIN));                    // ch0 untouched by jpeg teardown
}

// ---- near-simultaneous snapshots share one capture --------------------------
void test_snapshot_cache() {
    Rig r;
    auto m = r.mgr.acquire(ConsumerType::Rtsp);
    std::vector<uint8_t> a, b; std::string err;
    MCHECK(r.mgr.snapshot(a, err) && r.mgr.snapshot(b, err));
    MCHECK(a == b);
    MCHECK(r.log.count("jpeg.capture") == 1);                // second request hit the cache
    MCHECK(r.mgr.stats().jpeg_captures == 1);
}

// ---- >= 20 snapshots: no leak, bounded resources ----------------------------
void test_repeated_snapshots() {
    Rig r;
    r.mgr.configure_jpeg(JpegParams{}, 0 /*no cache*/, 2000, &r.jpeg_timer);
    auto m = r.mgr.acquire(ConsumerType::Rtsp);
    std::vector<uint8_t> jpg; std::string err;
    for (int i = 0; i < 20; ++i) MCHECK(r.mgr.snapshot(jpg, err));
    MCHECK(r.mgr.stats().jpeg_captures == 20);
    MCHECK(r.log.count("jpeg.create") == 1);                 // ONE encoder, not one per request
    r.mgr.on_jpeg_grace();
    MCHECK(r.log.count("jpeg.destroy") == 1);
}

// ---- failure paths: a broken jpeg/sub must not touch the main stream --------
void test_failures_do_not_kill_main() {
    Rig r;
    auto m = r.mgr.acquire(ConsumerType::Rtsp);
    r.platform.fail_at = FakePlatform::FailAt::Jpeg; r.platform.fail_times = 1;
    std::vector<uint8_t> jpg; std::string err;
    MCHECK(!r.mgr.snapshot(jpg, err));
    MCHECK(r.mgr.state() == State::Active && r.mgr.unit_active(UNIT_MAIN));
    MCHECK(r.mgr.snapshot(jpg, err));                        // recovers on the next try

    r.platform.fail_at = FakePlatform::FailAt::Encoder; r.platform.fail_times = 1;
    Result res;
    auto s = r.mgr.acquire_unit(UNIT_SUB, ConsumerType::Rtsp, &res);
    MCHECK(!s.active() && !res);
    MCHECK(r.mgr.state() == State::Active && r.mgr.unit_active(UNIT_MAIN));
    auto s2 = r.mgr.acquire_unit(UNIT_SUB, ConsumerType::Rtsp);
    MCHECK(s2.active());

    // capture failure is counted, not fatal
    r.platform.jpeg_capture_fails = true;
    r.mgr.on_jpeg_grace();                                   // drop the cached encoder+cache
    MCHECK(!r.mgr.snapshot(jpg, err));
    MCHECK(r.mgr.stats().jpeg_failures >= 1);
    MCHECK(r.mgr.unit_active(UNIT_MAIN));
}

// ---- a jpeg-start failure from cold must not leave the base running ---------
void test_jpeg_fail_from_cold_rolls_base_back() {
    Rig r;
    r.platform.fail_at = FakePlatform::FailAt::Jpeg; r.platform.fail_times = 1;
    std::vector<uint8_t> jpg; std::string err;
    MCHECK(!r.mgr.snapshot(jpg, err));
    MCHECK(r.mgr.state() == State::Failed);
    MCHECK(r.log.count("platform.bring_up") == r.log.count("platform.tear_down"));
    // FAILED is sticky, and the snapshot door is no exception: a rolled-back
    // base stays down until the daemon restarts, even though a second attempt
    // would succeed here. Widening note: this makes a JPEG-encoder failure
    // terminal for the whole base too, not just for snapshots.
    MCHECK(!r.mgr.snapshot(jpg, err));
    MCHECK(r.mgr.state() == State::Failed);
}

// ---- unconfigured units refuse cleanly --------------------------------------
void test_unconfigured_refused() {
    Rig r(false, false);
    Result res;
    auto s = r.mgr.acquire_unit(UNIT_SUB, ConsumerType::Rtsp, &res);
    MCHECK(!s.active() && res.status == Status::Unsupported);
    std::vector<uint8_t> jpg; std::string err;
    MCHECK(r.mgr.snapshot(jpg, err).status == Status::Unsupported);
    MCHECK(r.mgr.state() == State::ColdIdle);                // nothing was started for a refusal
}

// ---- update_sub_stream: applies cold, restarts live, disable stops ----------
void test_sub_update() {
    Rig r;
    auto s = r.mgr.acquire_unit(UNIT_SUB, ConsumerType::Rtsp);
    std::string err;
    EffectiveStream ns = Rig::mk(896, 512, 12, 768);
    MCHECK(r.mgr.update_sub_stream(ns, true, err));
    MCHECK(r.mgr.stats().sub_restart_count == 1);
    MCHECK(r.platform.last_stream.width == 896);
    MCHECK(r.mgr.stream_unit(UNIT_SUB).bitrate_kbps == 768);
    MCHECK(r.mgr.state() == State::Active);                  // base untouched by a sub restart

    MCHECK(r.mgr.update_sub_stream(ns, false, err));         // disable stops the unit
    MCHECK(!r.mgr.unit_active(UNIT_SUB));
    Result res;
    auto s2 = r.mgr.acquire_unit(UNIT_SUB, ConsumerType::Rtsp, &res);
    MCHECK(!s2.active() && res.status == Status::Unsupported);
}

// ---- a base restart brings back every unit that still has demand ------------
void test_base_restart_restores_sub() {
    Rig r;
    auto m = r.mgr.acquire(ConsumerType::Rtsp);
    auto s = r.mgr.acquire_unit(UNIT_SUB, ConsumerType::Rtsp);
    std::string err;
    EffectiveStream ns = r.mgr.stream(); ns.fps = 15;
    MCHECK(r.mgr.update_stream(ns, true, err));
    MCHECK(r.mgr.unit_active(UNIT_MAIN) && r.mgr.unit_active(UNIT_SUB));  // sub survived the restart
    MCHECK(r.mgr.stream().fps == 15 && r.mgr.stream_unit(UNIT_SUB).width == 640);
}

// ch0 + ch1, then ch0 leaves while ch1 stays: the base and ch1 keep running,
// ch0 winds down on its own grace (the reverse of test_main_plus_sub).
void test_main_winds_down_while_sub_runs() {
    Rig r;
    auto m = r.mgr.acquire(ConsumerType::Rtsp);
    auto sub = r.mgr.acquire_unit(UNIT_SUB, ConsumerType::Rtsp);
    MCHECK(r.mgr.unit_active(UNIT_MAIN) && r.mgr.unit_active(UNIT_SUB));
    m.release();                                       // last ch0 consumer leaves; ch1 stays
    MCHECK(r.mgr.state() == State::Active);            // base untouched
    MCHECK(r.main_timer.armed && r.mgr.unit_active(UNIT_MAIN));  // ch0 warm until its grace
    r.mgr.on_unit_grace(UNIT_MAIN);
    MCHECK(!r.mgr.unit_active(UNIT_MAIN) && r.mgr.unit_active(UNIT_SUB));  // ch0 gone, ch1 runs
    MCHECK(r.mgr.state() == State::Active && r.log.count("platform.tear_down") == 0);
}

// AI base demand: sensor/ISP up, no encoder started at all.
void test_ai_base_demand() {
    Rig r;
    auto ai = r.mgr.acquire_base(ConsumerType::Ai);
    MCHECK(ai.active() && ai.unit() == UNIT_AI);
    MCHECK(r.mgr.state() == State::Active);
    MCHECK(r.log.count("platform.bring_up") == 1 && r.log.count("enc.create") == 0);   // base only
    MCHECK(r.mgr.stats().unit_active[UNIT_AI]);
    // a main consumer can still join the same base without a second bring-up
    auto m = r.mgr.acquire(ConsumerType::Rtsp);
    MCHECK(m.active() && r.mgr.unit_active(UNIT_MAIN) && r.log.count("platform.bring_up") == 1);
    m.release();
    MCHECK(r.mgr.state() == State::Active);            // AI still holds the base
    ai.release();
    MCHECK(r.mgr.state() == State::GraceIdle);
    r.mgr.on_grace_timeout();
    MCHECK(r.mgr.state() == State::ColdIdle && r.log.count("platform.tear_down") == 1);
}

} // namespace

void run_multistream_tests() {
    test_sub_only();
    test_main_plus_sub();
    test_base_grace_keeps_units_warm();
    test_snapshot_from_cold();
    test_snapshot_during_main();
    test_snapshot_cache();
    test_repeated_snapshots();
    test_failures_do_not_kill_main();
    test_jpeg_fail_from_cold_rolls_base_back();
    test_unconfigured_refused();
    test_sub_update();
    test_base_restart_restores_sub();
    test_main_winds_down_while_sub_runs();
    test_ai_base_demand();
}
