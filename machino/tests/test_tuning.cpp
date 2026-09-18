// M7 image/latency policy and bounded live-queue tests.
#include "core/media/tuning_service.hpp"
#include "fake_platform.hpp"
#include <cstdio>

using namespace machino;
using namespace machino::lifecycle;
using namespace machino::media;
using namespace machino::test;

extern int g_fail_ext, g_pass_ext;
#define TCHECK(cond) do { if (cond) { ++g_pass_ext; } else { ++g_fail_ext; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

namespace {

struct Rig {
    CallLog log; FakePlatform platform{log}; FakeTimer timer; StreamHub hub;
    EffectiveStream stream{}; LifecycleConfig lc{}; PipelineManager mgr;
    Rig() : stream(mk_stream()), mgr(platform, stream, mk_lc(), timer, hub) {}
    static EffectiveStream mk_stream() { EffectiveStream s; s.width=1920; s.height=1080; s.native_width=1920; s.native_height=1080; s.fps=20; s.gop=40; s.buffers=2; return s; }
    static LifecycleConfig mk_lc() { LifecycleConfig c; c.poll_timeout_ms=10; c.idle_grace_ms=10; return c; }
};

void test_image_deferred_and_reapplied() {
    Rig r; ImageSettings im; LatencySettings lat;
    TuningService t(r.mgr, r.platform, r.hub, r.stream, im, lat);
    auto a = t.set_image(ImageControl::Brightness, 90);
    TCHECK(a.ok && a.deferred && r.log.count("platform.bring_up") == 0);
    auto bad = t.set_image(ImageControl::Wdr, 1);
    TCHECK(!bad.ok && bad.mode == ApplyMode::Unsupported);
    {
        auto d = r.mgr.acquire(ConsumerType::Rtsp);
        TCHECK(d.active() && r.log.count("image.set.brightness@90") == 1);
        auto live = t.set_image(ImageControl::Brightness, 80);
        TCHECK(live.ok && !live.deferred && live.effective == 80 && r.platform.image_control.values[(int)ImageControl::Brightness] == 80);
        ExposureReadback e; TCHECK(t.exposure(e) && e.available && e.stable && e.luma == 100);
    }
    r.mgr.on_grace_timeout();
    { auto d = r.mgr.acquire(ConsumerType::Rtsp); TCHECK(d.active() && r.log.count("image.set.brightness@80") == 2); }
}

void test_latency_profiles_and_overrides() {
    Rig r; ImageSettings im; LatencySettings lat; lat.profile = LatencyProfile::Low; lat.gop = 7; lat.consumer_queue_depth = 3;
    TuningService t(r.mgr, r.platform, r.hub, r.stream, im, lat);
    TuningState s = t.state();
    TCHECK(s.latency.profile == LatencyProfile::Low && s.latency.gop == 7);  // explicit > preset
    TCHECK(s.latency.framesource_buffers == 1 && s.latency.encoder_buffers == 1 && s.latency.consumer_queue_depth == 3);
    TCHECK(r.hub.default_depth() == 3 && r.mgr.state() == State::ColdIdle);
    auto d = r.mgr.acquire(ConsumerType::Rtsp);
    TCHECK(d.active() && r.platform.last_stream.gop == 7 && r.platform.last_stream.buffers == 1 && r.platform.last_stream.encoder_buffers == 1);
    int before = (int)r.mgr.stats().restart_count;
    auto normal = t.set_latency_profile(LatencyProfile::Normal);
    // Explicit GOP stays 7, while the low profile's shallow hardware buffers
    // return to normal in one controlled restart.
    TCHECK(normal.ok && r.mgr.stream().gop == 7 && r.mgr.stream().buffers == 2);
    TCHECK((int)r.mgr.stats().restart_count == before + 1 && r.mgr.stats().total_demand == 1);
    auto g = t.set_gop(10);
    TCHECK(g.ok && r.log.count("enc.set_gop@10") == 1 && r.mgr.stream().gop == 10);
    TCHECK(!t.set_gop(0).ok && !t.set_queue_depth(0).ok);
}

void test_bounded_stale_policy() {
    StreamHub h; auto sink = h.subscribe(2);
    auto make = [](unsigned seq, bool key) { auto a = std::make_shared<AccessUnit>(); a->seq=seq; a->key=key; a->data.push_back(key ? 0x65 : 0x41); return a; };
    h.publish(make(0, true)); AuPtr out; bool disc=false;
    TCHECK(sink->pop(out, 1, &disc) && out->seq == 0 && !disc);
    h.publish(make(1, false)); h.publish(make(2, false)); h.publish(make(3, false)); // overflow loses a reference
    TCHECK(!sink->pop(out, 1, &disc));                      // queued P tail discarded, not decoded
    h.publish(make(4, true));
    TCHECK(sink->pop(out, 1, &disc) && out->seq == 4 && out->key && disc);
    TCHECK(sink->dropped() >= 3 && sink->depth() == 2);
}

void test_idr_request() {
    Rig r; auto d = r.mgr.acquire(ConsumerType::Rtsp);
    r.mgr.request_idr();
    TCHECK(d.active() && r.log.count("enc.request_idr") == 1);
}

} // namespace


// Regression: the Ingenic encoder keeps reporting the previous GOP until the
// next GOP boundary. That pending read-back must never become the "effective"
// value - otherwise /state reports the old GOP forever and a later pipeline
// restart recreates the channel with it, silently undoing the change.
void test_gop_pending_readback_is_not_effective() {
    Rig r; r.platform.gop_readback_stale = true;
    ImageSettings im; LatencySettings lat;
    media::TuningService t(r.mgr, r.platform, r.hub, Rig::mk_stream(), im, lat);
    auto d = r.mgr.acquire(ConsumerType::Rtsp);
    TCHECK(d.active() && r.platform.last_encoder_stream.gop == 40);
    auto g = t.set_gop(15);
    TCHECK(g.ok && g.mode == ApplyMode::Live);
    TCHECK(t.state().latency.gop == 15);            // not the stale 40
    TCHECK(r.mgr.stream().gop == 15);               // survives into the next start
    auto g2 = t.set_gop(8);
    TCHECK(g2.ok && t.state().latency.gop == 8 && r.mgr.stream().gop == 8);
    // a restart must use the requested GOP, never the pending read-back
    std::string err; EffectiveStream s = r.mgr.stream();
    TCHECK(r.mgr.update_stream(s, true, err));
    TCHECK(r.platform.last_encoder_stream.gop == 8 && r.platform.last_stream.gop == 8);
}

void run_tuning_tests() {
    test_gop_pending_readback_is_not_effective();
    test_image_deferred_and_reapplied();
    test_latency_profiles_and_overrides();
    test_bounded_stale_policy();
    test_idr_request();
}
