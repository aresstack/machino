// Performance/power tests: PerformanceService over the PipelineManager with
// fake platform, fake power control and fake process stats.
#include "core/hw/registry.hpp"
#include "core/hw/resolve.hpp"
#include "core/lifecycle/pipeline_manager.hpp"
#include "core/power/performance_service.hpp"
#include "core/stream_hub.hpp"
#include "fake_platform.hpp"
#include "fake_power.hpp"

#include <cstdio>

using namespace machino;
using namespace machino::lifecycle;
using namespace machino::power;
using namespace machino::test;

extern int g_fail_ext, g_pass_ext;
#define PCHECK(cond) do { if (cond) { ++g_pass_ext; } else { ++g_fail_ext; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

namespace {

// A registry with a sensor that has three verified operating points and a
// board offering presets.
hw::ResolvedHardware make_hw(bool presets, bool allow_unverified = false) {
    hw::Registry r;
    r.add_platform({"fake", "f", "m"});
    hw::SensorDescriptor s; s.model = "s"; s.interface = hw::SensorInterface::MipiCsi; s.native_width = 1920; s.native_height = 1080;
    s.modes = { {1920, 1080, 20}, {1920, 1080, 15}, {1920, 1080, 10} };
    r.add_sensor(s);
    hw::BoardProfile b; b.board_id = "b"; b.platform = "fake-m"; b.sensor = "s";
    b.wiring.i2c_bus = 0; b.wiring.i2c_addr = 0x10; b.wiring.mclk = 0; b.default_mode = hw::SensorMode{1920, 1080, 20};
    if (presets) { b.presets.balanced_fps = 15; b.presets.battery_fps = 10; b.presets.battery_bitrate = 1200; }
    r.add_board(b);
    hw::UserHardwareConfig u; u.board_id = "b"; u.allow_unverified_mode = allow_unverified;
    hw::ResolvedHardware hw; std::string err;
    bool ok = hw::resolve_hardware(u, r, {}, hw, err);
    PCHECK(ok);
    return hw;
}

struct Rig {
    CallLog log; FakePowerControl power; FakePlatform platform{log, &power}; FakeTimer timer; StreamHub hub; FakeStats stats;
    hw::ResolvedHardware hw; EffectiveStream stream; StreamConfig video; LifecycleConfig lc;
    PipelineManager mgr; PerformanceService svc;
    explicit Rig(bool presets = true, bool allow_unverified = false)
        : hw(make_hw(presets, allow_unverified)), stream(mk_stream(hw)), mgr(platform, stream, mk_lc(), timer, hub),
          svc(mgr, platform, stats, hw, video) {}
    static EffectiveStream mk_stream(const hw::ResolvedHardware& h) { StreamConfig v; return effective_stream(v, h); }
    static LifecycleConfig mk_lc() { LifecycleConfig c; c.idle_grace_ms = 1000; c.poll_timeout_ms = 10; return c; }
};

void test_capabilities() {
    Rig r;
    CapabilitySet c = r.svc.capabilities();
    PCHECK(c.video.bitrate.support == Cap::Supported && c.video.bitrate.apply == ApplyMode::Live);
    PCHECK(c.video.fps.apply == ApplyMode::PipelineRestart);
    PCHECK(c.sensor.fps.min == 10 && c.sensor.fps.max == 20);          // from verified modes only
    PCHECK(c.isp.performance.support == Cap::Unsupported && c.isp.performance.readable);
    PCHECK(c.power.cpu_frequency.support == Cap::Unsupported);
    PCHECK(c.ai.available == Cap::Supported);                          // fake advertises the motion backend
    Rig q; q.power.cpu_support = Cap::Supported; q.platform.power_ptr = &q.power;
    // service captured caps at construction with cpu unsupported; a fresh service sees Supported
    PerformanceService svc2(q.mgr, q.platform, q.stats, q.hw, q.video);
    PCHECK(svc2.capabilities().power.cpu_frequency.support == Cap::Supported);
    PCHECK(std::string(cap_name(Cap::Unknown)) == "unknown" && std::string(apply_mode_name(ApplyMode::PipelineRestart)) == "pipeline-restart");
}

void test_profile_resolution() {
    Rig r;                                                           // presets 15/10
    ApplyResult a = r.svc.apply_profile(Profile::Battery);
    PCHECK(a.ok && a.deferred && r.mgr.stream().fps == 10 && r.mgr.stream().bitrate_kbps == 1200);
    PCHECK(r.svc.effective_state().profile == Profile::Battery);
    a = r.svc.apply_profile(Profile::Balanced);
    PCHECK(a.ok && r.mgr.stream().fps == 15 && r.mgr.stream().bitrate_kbps == 3000);
    a = r.svc.apply_profile(Profile::Performance);
    PCHECK(a.ok && r.mgr.stream().fps == 20);
    // no presets: derived from verified points (next lower / lowest)
    Rig q(false);
    PCHECK(q.svc.apply_profile(Profile::Balanced).ok && q.mgr.stream().fps == 15);
    PCHECK(q.svc.apply_profile(Profile::Battery).ok && q.mgr.stream().fps == 10);
    // single verified point: battery == performance, said so
    hw::Registry rr; rr.add_platform({"fake", "f", "m"});
    hw::SensorDescriptor s; s.model = "s"; s.interface = hw::SensorInterface::MipiCsi; s.modes = { {640, 480, 30} }; rr.add_sensor(s);
    hw::UserHardwareConfig u; u.platform = "fake-m"; u.sensor = "s"; u.wiring.i2c_bus = 0; u.wiring.i2c_addr = 1; u.wiring.mclk = 0;
    hw::ResolvedHardware h; std::string err; PCHECK(hw::resolve_hardware(u, rr, {}, h, err));
    CallLog log; FakePlatform fp(log); FakeTimer ft; StreamHub hub; FakeStats fs; StreamConfig v;
    LifecycleConfig lc; PipelineManager m(fp, effective_stream(v, h), lc, ft, hub);
    PerformanceService svc(m, fp, fs, h, v);
    ApplyResult b = svc.apply_profile(Profile::Battery);
    PCHECK(b.ok && m.stream().fps == 30 && b.message.find("no lower verified") != std::string::npos);
}

// A profile is one operating point: if any part of it cannot be applied, none
// of it is. Otherwise a rejected profile still moves the camera - here the fps
// would change while the bitrate stays behind, and /state would say "custom".
void test_profile_is_transactional() {
    hw::Registry rr; rr.add_platform({"fake", "f", "m"});
    hw::SensorDescriptor s; s.model = "s"; s.interface = hw::SensorInterface::MipiCsi;
    s.native_width = 1920; s.native_height = 1080; s.modes = { {1920, 1080, 20}, {1920, 1080, 10} };
    rr.add_sensor(s);
    hw::BoardProfile b; b.board_id = "b"; b.platform = "fake-m"; b.sensor = "s";
    b.wiring.i2c_bus = 0; b.wiring.i2c_addr = 0x10; b.wiring.mclk = 0; b.default_mode = hw::SensorMode{1920, 1080, 20};
    b.presets.battery_fps = 10;            // the sensor can do this
    b.presets.battery_bitrate = 999999;    // the encoder cannot do this
    rr.add_board(b);
    hw::UserHardwareConfig u; u.board_id = "b";
    hw::ResolvedHardware h; std::string err; PCHECK(hw::resolve_hardware(u, rr, {}, h, err));
    CallLog log; FakePlatform fp(log); FakeTimer ft; StreamHub hub; FakeStats fs; StreamConfig v;
    LifecycleConfig lc; PipelineManager m(fp, effective_stream(v, h), lc, ft, hub);
    PerformanceService svc(m, fp, fs, h, v);

    const int fps0 = m.stream().fps, kbps0 = m.stream().bitrate_kbps;
    ApplyResult a = svc.apply_profile(Profile::Battery);
    PCHECK(!a.ok && a.requested == 999999);
    PCHECK(a.message.find("not applied (nothing changed)") != std::string::npos);
    PCHECK(m.stream().fps == fps0 && m.stream().bitrate_kbps == kbps0);   // the valid half was not applied either
    PCHECK(svc.effective_state().profile == Profile::Performance);        // and the profile did not move
    PCHECK(log.count("platform.set_sensor_fps@10") == 0);

    // the same service applies a profile whose parts are all within reach
    PCHECK(svc.apply_profile(Profile::Balanced).ok && m.stream().fps == 10);
    PCHECK(svc.effective_state().profile == Profile::Balanced);
}

// The half that only fails at the hardware. Validation cannot see it coming,
// so the profile name must not claim the box is at that operating point.
void test_profile_partial_apply_is_not_ok() {
    Rig r;
    auto d = r.mgr.acquire(ConsumerType::Rtsp);          // live path: the setter reaches the platform
    PCHECK(d.active());
    r.platform.sensor_fps_runtime_error = true;          // capabilities still say supported
    ApplyResult a = r.svc.apply_profile(Profile::Battery);
    PCHECK(!a.ok);
    PCHECK(a.message.find("only partially applied") != std::string::npos);
    PCHECK(a.message.find("sensor fps") != std::string::npos);
    PCHECK(r.svc.effective_state().profile == Profile::Custom);   // the name must not lie
    PCHECK(r.mgr.stream().fps == 10);                             // what did take effect is reported as it is

    // A platform without any sensor rate control is a different case: there the
    // stream rate is the operating point, so the profile applies completely.
    Rig q; q.platform.sensor_fps_supported = false;
    PerformanceService svc2(q.mgr, q.platform, q.stats, q.hw, q.video);
    PCHECK(svc2.apply_profile(Profile::Battery).ok);
    PCHECK(q.mgr.stream().fps == 10 && svc2.effective_state().profile == Profile::Battery);

    // A sensor fps outside the verified range is caught before anything moves.
    Rig w;
    ApplyResult c = w.svc.set_sensor_fps(99);
    PCHECK(!c.ok && c.message.find("outside known range") != std::string::npos);
}

void test_user_override_wins_and_invalid_rejected() {
    Rig r;
    PCHECK(r.svc.apply_profile(Profile::Battery).ok && r.mgr.stream().fps == 10);
    ApplyResult a = r.svc.set_stream_fps(15);                        // explicit user value after a profile
    PCHECK(a.ok && r.mgr.stream().fps == 15 && r.svc.effective_state().profile == Profile::Custom);
    // invalid values are rejected, never clamped
    PCHECK(!r.svc.set_stream_fps(0).ok);
    PCHECK(!r.svc.set_stream_fps(12).ok && r.mgr.stream().fps == 15);          // not a verified point
    int before_kbps = r.mgr.stream().bitrate_kbps;
    PCHECK(!r.svc.set_bitrate(50).ok && r.mgr.stream().bitrate_kbps == before_kbps);  // below known min (100): rejected, not clamped
    PCHECK(!r.svc.set_bitrate(50000).ok);                                        // above known max
    PCHECK(!r.svc.set_sensor_fps(-1).ok);
    PCHECK(!r.svc.set_sensor_fps(25).ok);                                        // outside [10..20]
    // unverified point allowed only with the explicit opt-in
    Rig q(true, true);
    PCHECK(q.svc.set_stream_fps(12).ok && q.mgr.stream().fps == 12);
}

void test_live_and_restart_paths() {
    Rig r;
    auto d = r.mgr.acquire(ConsumerType::Rtsp);
    PCHECK(d.active() && r.mgr.state() == State::Active);
    int bring_ups = r.log.count("platform.bring_up");
    // LIVE bitrate: no restart, effective read back
    ApplyResult b = r.svc.set_bitrate(1500);
    PCHECK(b.ok && b.mode == ApplyMode::Live && !b.deferred && b.effective == 1500);
    PCHECK(r.log.count("enc.set_bitrate@1500") == 1 && r.log.count("platform.bring_up") == bring_ups);
    PCHECK(r.mgr.stream().bitrate_kbps == 1500 && r.svc.effective_state().bitrate_kbps == 1500);
    // LIVE sensor fps with hardware read-back
    ApplyResult sf = r.svc.set_sensor_fps(15);
    PCHECK(sf.ok && sf.mode == ApplyMode::Live && sf.effective == 15);
    EffectiveState es = r.svc.effective_state();
    PCHECK(es.sensor_fps_requested == 15 && es.sensor_fps_effective == 15 && es.sensor_fps_readback);
    // PIPELINE_RESTART stream fps: restart with demand preserved
    ApplyResult f = r.svc.set_stream_fps(10);
    PCHECK(f.ok && f.mode == ApplyMode::PipelineRestart && !f.deferred);
    PCHECK(r.log.count("platform.bring_up") == bring_ups + 1 && r.log.count("platform.tear_down") == 1);
    PCHECK(r.log.count("fs.create") == 2 && r.platform.last_stream.fps == 10);
    PCHECK(r.mgr.state() == State::Active && r.mgr.stats().total_demand == 1 && r.mgr.stats().restart_count == 1);
    PCHECK(r.mgr.stats().generation == 2);
    // sensor fps target re-applied after the restart
    PCHECK(r.log.count("platform.set_sensor_fps@15") >= 2);
    // encoder without live bitrate -> restart path
    Rig q; q.platform.live_bitrate = false;
    PerformanceService svc2(q.mgr, q.platform, q.stats, q.hw, q.video);
    auto e = q.mgr.acquire(ConsumerType::Rtsp);
    ApplyResult nb = svc2.set_bitrate(2000);
    PCHECK(nb.ok && nb.mode == ApplyMode::PipelineRestart && q.log.count("platform.bring_up") == 2 && q.mgr.stream().bitrate_kbps == 2000);
}

void test_cold_setting_does_not_start() {
    Rig r;
    PCHECK(r.mgr.state() == State::ColdIdle);
    ApplyResult a = r.svc.apply_profile(Profile::Battery);
    PCHECK(a.ok && a.deferred);
    ApplyResult b = r.svc.set_bitrate(900);
    PCHECK(b.ok && b.deferred);
    PCHECK(r.log.count("platform.bring_up") == 0 && r.mgr.state() == State::ColdIdle);
    // next consumer starts directly with the battery settings
    auto d = r.mgr.acquire(ConsumerType::Rtsp);
    PCHECK(r.platform.last_stream.fps == 10 && r.platform.last_stream.bitrate_kbps == 900);
    PCHECK(r.log.count("platform.set_sensor_fps@10") == 1);        // sensor rate applied at start
    PCHECK(r.svc.effective_state().sensor_fps_effective == 10);
}

void test_failed_restart_deterministic() {
    Rig r;
    auto d = r.mgr.acquire(ConsumerType::Rtsp);
    r.platform.fail_at = FakePlatform::FailAt::Bind; r.platform.fail_times = 1;
    ApplyResult f = r.svc.set_stream_fps(10);
    PCHECK(!f.ok && f.mode == ApplyMode::PipelineRestart && f.message.find("bind") != std::string::npos);
    PCHECK(r.mgr.state() == State::Failed && !r.platform.is_up());
    PCHECK(r.mgr.stats().total_demand == 1);                          // demand not lost
    PCHECK(r.log.count("platform.bring_up") == 2 && r.log.count("platform.tear_down") == 2);
    // a new consumer may try again (no automatic loop happened)
    auto e = r.mgr.acquire(ConsumerType::Rtsp);
    PCHECK(e.active() && r.mgr.state() == State::Active && r.log.count("platform.bring_up") == 3);
}

void test_requested_vs_effective() {
    Rig r; r.platform.sensor_fps_supported = false;
    PerformanceService svc(r.mgr, r.platform, r.stats, r.hw, r.video);
    auto d = r.mgr.acquire(ConsumerType::Rtsp);
    ApplyResult a = svc.set_sensor_fps(10);
    PCHECK(!a.ok && a.mode == ApplyMode::Unsupported);              // not silently "saved"
    EffectiveState e = svc.effective_state();
    PCHECK(e.sensor_fps_requested == 10 && !e.sensor_fps_readback && e.sensor_fps_effective == -1);
    PCHECK(e.sensor_fps_mode == ApplyMode::Unsupported);
    // supported: effective differs from requested when the hardware says so
    Rig q; auto dq = q.mgr.acquire(ConsumerType::Rtsp);
    q.platform.sensor_fps_effective = 20;
    EffectiveState eq = q.svc.effective_state();
    PCHECK(eq.sensor_fps_requested == 20 && eq.sensor_fps_effective == 20 && eq.sensor_fps_readback);
    q.platform.sensor_fps_effective = 19;                             // hardware reports something else
    PCHECK(q.svc.effective_state().sensor_fps_effective == 19);
}

void test_power_levels_and_telemetry() {
    Rig r;
    PCHECK(r.svc.set_isp_performance(PerfLevel::Auto).ok);
    ApplyResult a = r.svc.set_isp_performance(PerfLevel::Low);
    PCHECK(!a.ok && a.mode == ApplyMode::Unsupported);
    PCHECK(!r.svc.set_cpu_performance(PerfLevel::High).ok);
    r.power.cpu_support = Cap::Supported;
    PCHECK(r.svc.set_cpu_performance(PerfLevel::Low).ok && r.power.cpu_level == PerfLevel::Low);
    // telemetry: explicit unavailability
    Telemetry t = r.svc.telemetry();
    PCHECK(t.state == State::ColdIdle && !t.measured_encoded_fps.available && !t.effective_sensor_fps.available);
    PCHECK(t.rss_kb.available && t.rss_kb.value == 2048 && t.threads.value == 3 && t.cpu_percent.available);
    PCHECK(t.isp_clock_hz.available && t.encoder_clock_hz.available);
    r.stats.available = false; r.power.clocks_readable = false;
    Telemetry u = r.svc.telemetry();
    PCHECK(!u.rss_kb.available && !u.threads.available && !u.cpu_percent.available && !u.isp_clock_hz.available);
    // config apply path with "auto" levels touches nothing
    PerformanceConfig pc; StreamConfig v; v.bitrate_kbps = 3000;
    size_t before = r.power.calls.size();
    r.svc.apply_config(pc, v);
    PCHECK(r.power.calls.size() == before);
}

} // namespace

void run_power_tests() {
    test_capabilities();
    test_profile_resolution();
    test_profile_is_transactional();
    test_profile_partial_apply_is_not_ok();
    test_user_override_wins_and_invalid_rejected();
    test_live_and_restart_paths();
    test_cold_setting_does_not_start();
    test_failed_restart_deterministic();
    test_requested_vs_effective();
    test_power_levels_and_telemetry();
}
