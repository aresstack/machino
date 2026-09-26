// M9: detection/AI service. What matters: the detector is a CONSUMER, never a
// second media owner. It holds a base-only demand (sensor/ISP up, no encoder),
// runs at its own cadence, degrades to Error without ever touching video or the
// daemon, and emits events on detections and motion transitions - not once per
// analysed frame.
#include "core/detection/detection_service.hpp"
#include "core/detection/latest_frame_slot.hpp"
#include "core/events.hpp"
#include "core/lifecycle/pipeline_manager.hpp"
#include "core/stream_hub.hpp"
#include "fake_platform.hpp"

#include <chrono>
#include <cstdio>
#include <thread>

using namespace machino;
using namespace machino::detection;
using namespace machino::lifecycle;
using namespace machino::test;

extern int g_fail_ext, g_pass_ext;
#define DCHECK(cond) do { if (cond) { ++g_pass_ext; } else { ++g_fail_ext; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

namespace {

struct Rig {
    CallLog log;
    FakePlatform platform{log};
    FakeTimer base_timer, main_timer;
    StreamHub hub;
    EffectiveStream main_s;
    LifecycleConfig lc;
    PipelineManager mgr;
    EventBus bus;

    static EffectiveStream mk() {
        EffectiveStream s; s.width = 1920; s.height = 1080; s.fps = 20; s.bitrate_kbps = 3000;
        s.native_width = 1920; s.native_height = 1080; return s;
    }
    static LifecycleConfig mk_lc() { LifecycleConfig c; c.idle_grace_ms = 1000; c.poll_timeout_ms = 10; return c; }

    Rig() : main_s(mk()), lc(mk_lc()), mgr(platform, main_s, lc, base_timer, hub) {
        mgr.set_unit_grace_timer(UNIT_MAIN, &main_timer);
    }
};

template <class F>
static bool wait_for(F f, int ms = 2000) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (f()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return f();
}

static AiConfig ai(bool on, const char* det = "motion", int fps = 5) {
    AiConfig c; c.enabled = on; c.detector = det; c.inference_fps = fps; return c;
}

// ---- disabled: no detector, no demand, base never comes up ------------------
void test_disabled_no_demand() {
    Rig r;
    DetectionService svc(r.mgr, r.platform, r.bus, ai(false));
    DCHECK(svc.state() == AiState::Disabled);
    DCHECK(r.mgr.state() == State::ColdIdle);
    DCHECK(r.log.count("platform.bring_up") == 0 && r.log.count("det.create") == 0);
}

// ---- enabled: base-only demand, NO encoder, detector started ---------------
void test_enabled_base_only() {
    Rig r;
    DetectionService svc(r.mgr, r.platform, r.bus, ai(true, "motion", 5));
    DCHECK(svc.state() == AiState::Active);
    DCHECK(r.mgr.state() == State::Active && r.mgr.unit_active(UNIT_AI));
    DCHECK(r.log.count("platform.bring_up") == 1);
    DCHECK(r.log.count("enc.create") == 0);                 // a detector needs the base, never an encoder
    DCHECK(r.log.count("det.create") == 1 && r.log.count("det.start") == 1);
    DCHECK(r.platform.last_detector.detector == "motion" && r.platform.last_detector.inference_fps == 5);
    svc.shutdown();
    DCHECK(r.log.count("det.stop") == 1 && r.log.count("det.destroy") == 1);
    DCHECK(r.mgr.state() == State::GraceIdle);              // base released when the AI demand went away
    DCHECK(r.log.count("platform.tear_down") == 0);        // still warm inside grace
}

// ---- backend unavailable: AI Error, base released, VIDEO UNAFFECTED --------
void test_unavailable_is_error_not_fatal() {
    Rig r;
    r.platform.detector_supported = false;
    DetectionService svc(r.mgr, r.platform, r.bus, ai(true));
    DCHECK(svc.state() == AiState::Error);
    DCHECK(r.log.count("det.create") == 0);
    DCHECK(r.mgr.state() == State::GraceIdle);              // AI demand released after the base briefly came up
    DCHECK(r.log.count("platform.bring_up") == 1 && r.log.count("platform.tear_down") == 0);
    // the daemon and the video path are untouched: a normal consumer still works
    auto m = r.mgr.acquire(ConsumerType::Rtsp);
    DCHECK(m.active() && r.mgr.unit_active(UNIT_MAIN));
    DCHECK(r.log.count("platform.bring_up") == 1);         // reused the warm base, no second bring-up
    DCHECK(r.log.count("enc.create") == 1);
}

// ---- backend start failure: Error, detector torn down, demand released ------
void test_start_failure_is_error() {
    Rig r;
    r.platform.detector_fail_start = true;
    DetectionService svc(r.mgr, r.platform, r.bus, ai(true));
    DCHECK(svc.state() == AiState::Error);
    DCHECK(r.log.count("det.create") == 1 && r.log.count("det.start") == 1);
    DCHECK(r.log.count("det.destroy") == 1 && r.log.count("det.stop") == 0);   // start never succeeded
    DCHECK(r.mgr.state() == State::GraceIdle);              // demand released, base winding down
}

// ---- events fire on detections and on the off-transition, never per frame --
void test_events_on_detection_not_empty_frames() {
    Rig r;
    auto sub = r.bus.subscribe(512);
    r.platform.detector_motion_frames = 3;                  // 3 results carry motion, then quiet forever
    DetectionService svc(r.mgr, r.platform, r.bus, ai(true, "motion", 30));
    DCHECK(svc.state() == AiState::Active);

    // wait until motion has been seen and then cleared (the off-transition ran)
    DCHECK(wait_for([&] { auto t = svc.telemetry(); return t.detections_total == 3 && !t.motion_now; }));
    auto t1 = svc.telemetry();

    // let many more quiet polls run: these must NOT emit anything
    unsigned c1 = t1.completed;
    DCHECK(wait_for([&] { return svc.telemetry().completed >= c1 + 5; }));

    int detev = 0, aiev = 0, boxed = 0; Event e;
    while (sub->pop(e)) {
        if (e.type == "detection") {
            ++detev;
            // §AP-NNA3: das Ereignis traegt die BOX -- ein Abonnent braucht
            // WO, nicht nur DASS. Werte wie die Attrappe sie setzt.
            if (e.data.find("\"box\"") != std::string::npos
                && e.data.find("\"w\":0.5") != std::string::npos)
                ++boxed;
        } else if (e.type == "ai") ++aiev;
    }
    DCHECK(aiev == 1);                                      // one lifecycle "ai" event on start
    DCHECK(detev == 4);                                     // 3 motion frames + 1 off-transition, no empty-frame spam
    DCHECK(boxed == 3);                                     // jede echte Detektion mit Box, die Off-Transition ohne
    DCHECK(t1.detections_total == 3);
    // Die gemessene Dauer der Attrappe (8 ms) landet in der Telemetrie.
    DCHECK(t1.last_infer_duration_ms == 8);
    DCHECK(t1.avg_infer_duration_ms > 0.0);
    svc.shutdown();
}

// ---- Wechsel motion -> person -> motion: sauberer Restart je Wechsel --------
void test_detector_switch_roundtrip() {
    Rig r;
    DetectionService svc(r.mgr, r.platform, r.bus, ai(true, "motion", 5));
    DCHECK(svc.state() == AiState::Active && r.platform.last_detector.detector == "motion");
    DCHECK(bool(svc.set_detector("person")));
    DCHECK(svc.state() == AiState::Active && r.platform.last_detector.detector == "person");
    DCHECK(bool(svc.set_detector("motion")));
    DCHECK(svc.state() == AiState::Active && r.platform.last_detector.detector == "motion");
    DCHECK(r.log.count("det.create") == 3 && r.log.count("det.destroy") == 2);
    DCHECK(r.log.count("platform.bring_up") == 1);          // die Basis blinzelt nie
    svc.shutdown();
}

// ---- no activity is normal: timeouts are neither completed nor failed -------
void test_timeout_is_not_failure() {
    Rig r;
    r.platform.detector_motion_frames = -1;                 // detector always times out (nothing to report)
    DetectionService svc(r.mgr, r.platform, r.bus, ai(true));
    DCHECK(svc.state() == AiState::Active);
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    auto t = svc.telemetry();
    DCHECK(t.completed == 0 && t.failed == 0);
    DCHECK(svc.state() == AiState::Active);                 // still healthy
    svc.shutdown();
}

// ---- changing the cadence restarts the detector cleanly --------------------
void test_fps_change_restarts() {
    Rig r;
    DetectionService svc(r.mgr, r.platform, r.bus, ai(true, "motion", 5));
    DCHECK(svc.state() == AiState::Active && r.platform.last_detector.inference_fps == 5);
    DCHECK(bool(svc.set_inference_fps(15)));
    DCHECK(svc.state() == AiState::Active && r.platform.last_detector.inference_fps == 15);
    DCHECK(r.log.count("det.stop") == 1 && r.log.count("det.destroy") == 1);
    DCHECK(r.log.count("det.create") == 2 && r.log.count("det.start") == 2);
    DCHECK(r.log.count("platform.bring_up") == 1);         // the base never blinked across the restart
    DCHECK(!bool(svc.set_inference_fps(0)));                // out of range is refused
    svc.shutdown();
}

// ---- disable at runtime releases the demand and stops the detector ----------
void test_disable_releases_demand() {
    Rig r;
    DetectionService svc(r.mgr, r.platform, r.bus, ai(true));
    DCHECK(svc.state() == AiState::Active && r.mgr.unit_active(UNIT_AI));
    DCHECK(bool(svc.set_enabled(false)));
    DCHECK(svc.state() == AiState::Disabled);
    DCHECK(r.log.count("det.stop") == 1);
    DCHECK(r.mgr.state() == State::GraceIdle && !r.mgr.unit_active(UNIT_AI));
}

// ---- LatestFrameSlot: one in-flight, one replaceable, newest wins -----------
void test_latest_frame_slot() {
    LatestFrameSlot slot;
    const uint8_t a[4] = {1, 2, 3, 4}, b[4] = {5, 6, 7, 8};
    DCHECK(slot.offer(a, 4, 2, 2, 2, 100));                 // first offer accepted, nothing dropped
    DCHECK(!slot.offer(b, 4, 2, 2, 2, 200));                // pending frame replaced -> false + skip
    DCHECK(slot.skipped() == 1);
    std::vector<uint8_t> out; int w = 0, h = 0, st = 0; int64_t pts = 0;
    DCHECK(slot.take(out, w, h, st, pts, 50));
    DCHECK(out.size() == 4 && out[0] == 5 && pts == 200);   // the newest frame, not the first
    DCHECK(!slot.take(out, w, h, st, pts, 20));             // nothing pending -> timeout
    slot.close();
    DCHECK(!slot.offer(a, 4, 2, 2, 2, 1));                  // closed slot rejects offers
}

} // namespace

void run_detection_tests() {
    test_disabled_no_demand();
    test_enabled_base_only();
    test_unavailable_is_error_not_fatal();
    test_start_failure_is_error();
    test_events_on_detection_not_empty_frames();
    test_timeout_is_not_failure();
    test_fps_change_restarts();
    test_disable_releases_demand();
    test_detector_switch_roundtrip();
    test_latest_frame_slot();
}
