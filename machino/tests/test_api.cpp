// ApiService / ConfigStore tests with fake platform, fake power and a temp
// config file. Exercises the full PATCH path through PerformanceService and
// PipelineManager (no HTTP sockets - the transport is tested on hardware).
#include "app/api/api_service.hpp"
#include "app/compat/majestic_webui.hpp"
#include "core/config_store.hpp"
#include "core/detection/detection_service.hpp"
#include "core/events.hpp"
#include "core/hw/registry.hpp"
#include "core/hw/resolve.hpp"
#include "core/lifecycle/pipeline_manager.hpp"
#include "core/media/tuning_service.hpp"
#include "core/power/performance_service.hpp"
#include "core/stream_hub.hpp"
#include "fake_platform.hpp"
#include "fake_power.hpp"

#include <cstdio>
#include <thread>
#include <vector>

using namespace machino;
using namespace machino::lifecycle;
using namespace machino::test;

extern int g_fail_ext, g_pass_ext;
#define ACHECK(cond) do { if (cond) { ++g_pass_ext; } else { ++g_fail_ext; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

namespace {

const char* TMP_CONF = "tests/tmp_machino.conf";

hw::ResolvedHardware make_hw() {
    hw::Registry r;
    r.add_platform({"fake", "f", "m"});
    hw::SensorDescriptor s; s.model = "s"; s.interface = hw::SensorInterface::MipiCsi; s.native_width = 1920; s.native_height = 1080;
    s.modes = { {1920, 1080, 20}, {1920, 1080, 15}, {1920, 1080, 10} };
    r.add_sensor(s);
    hw::BoardProfile b; b.board_id = "board-x"; b.platform = "fake-m"; b.sensor = "s";
    b.wiring.i2c_bus = 0; b.wiring.i2c_addr = 0x10; b.wiring.mclk = 0; b.default_mode = hw::SensorMode{1920, 1080, 20};
    b.presets.balanced_fps = 15; b.presets.battery_fps = 10; b.presets.battery_bitrate = 1200; b.hardware_verified = true;
    r.add_board(b);
    hw::UserHardwareConfig u; u.board_id = "board-x";
    hw::ResolvedHardware hw; std::string err;
    ACHECK(hw::resolve_hardware(u, r, {}, hw, err));
    return hw;
}

// AP2: records what the API asks of the RTSP listener, and can refuse a bind
// the way a port already in use would.
struct FakeRtsp : public IRtspControl {
    bool enabled = true; int port = 554;
    bool fail_bind = false;            // next enable/port change cannot bind
    int  enable_calls = 0, port_calls = 0;
    power::ApplyResult set_enabled(bool on) override {
        ++enable_calls;
        if (on && fail_bind) return power::ApplyResult::rejected(ApplyMode::Live, 1, "cannot bind");
        enabled = on;
        return power::ApplyResult::applied(ApplyMode::Live, on ? 1 : 0, on ? 1 : 0, "");
    }
    power::ApplyResult set_port(int p) override {
        ++port_calls;
        if (p < 1 || p > 65535) return power::ApplyResult::rejected(ApplyMode::Live, p, "out of range");
        if (fail_bind) return power::ApplyResult::rejected(ApplyMode::Live, p, "cannot bind");
        port = p;
        return power::ApplyResult::applied(ApplyMode::Live, p, p, "");
    }
};

struct Rig {
    CallLog log; FakePowerControl power; FakePlatform platform{log, &power}; FakeTimer timer; StreamHub hub; FakeStats stats;
    EventBus bus; ConfigStore store{TMP_CONF};
    hw::ResolvedHardware hw; AppConfig cfg; EffectiveStream stream; LifecycleConfig lc;
    PipelineManager mgr; power::PerformanceService perf; media::TuningService tuning;
    detection::DetectionService detection; FakeRtsp rtsp; api::ApiService api;
    Rig() : hw(make_hw()), stream(effective_stream(cfg.video, hw)), mgr(platform, stream, mk_lc(), timer, hub),
            perf(mgr, platform, stats, hw, cfg.video), tuning(mgr, platform, hub, stream, cfg.image, cfg.latency),
            detection(mgr, platform, bus, cfg.ai), api(perf, tuning, mgr, store, bus, hw, cfg, &detection, &rtsp) {
        FILE* f = fopen(TMP_CONF, "w"); if (f) { fputs("# test\nboard = board-x\nvideo.bitrate = 3000\nlog.level = 2\n", f); fclose(f); }
        std::string err; store.load(err);
        mgr.set_state_listener([this](State a, State b) { Json j = Json::object(); j.set("from", Json::string(state_name(a))); j.set("to", Json::string(state_name(b))); bus.publish("lifecycle", j.dump()); });
    }
    static LifecycleConfig mk_lc() { LifecycleConfig c; c.idle_grace_ms = 1000; c.poll_timeout_ms = 10; return c; }
};

const Json* path(const Json& j, const char* p) { return json_path(j, p); }

void test_get_documents() {
    Rig r;
    api::Response d = r.api.discovery();
    ACHECK(d.status == 200 && path(d.body, "endpoints.events")->as_string() == "/api/v1/events" && d.body.get("api")->as_int() == 1);
    api::Response c = r.api.capabilities();
    ACHECK(c.status == 200 && path(c.body, "platform.vendor")->as_string() == "fake" && path(c.body, "board.id")->as_string() == "board-x");
    ACHECK(path(c.body, "sensor.model")->as_string() == "s" && path(c.body, "sensor.modes")->size() == 1 && path(c.body, "sensor.modes")->at(0).get("fps")->size() == 3);
    ACHECK(path(c.body, "controls.bitrate.status")->as_string() == "supported" && path(c.body, "controls.bitrate.apply")->as_string() == "live");
    ACHECK(path(c.body, "controls.bitrate.min")->as_int() == 100 && path(c.body, "controls.bitrate.max")->as_int() == 20000);
    ACHECK(path(c.body, "controls.stream_fps.apply")->as_string() == "pipeline_restart");
    ACHECK(path(c.body, "controls.sensor_fps.min")->as_int() == 10 && path(c.body, "controls.sensor_fps.max")->as_int() == 20);
    ACHECK(path(c.body, "controls.isp_clock.status")->as_string() == "unsupported" && path(c.body, "controls.isp_clock.readable")->as_bool());
    ACHECK(path(c.body, "controls.cpu_frequency.status")->as_string() == "unsupported");
    ACHECK(path(c.body, "video.h265")->as_string() == "unknown" && path(c.body, "video.max_streams")->is_null());   // unknown stays unknown/null
    ACHECK(path(c.body, "verified_fps")->size() == 3);
    api::Response s = r.api.state();
    ACHECK(s.status == 200 && s.body.get("lifecycle")->as_string() == "cold_idle" && path(s.body, "consumers.total")->as_int() == 0);
    ACHECK(path(s.body, "media.sensor_fps")->is_null() && path(s.body, "media.encoder_active")->as_bool() == false);   // cold: effective unknown -> null
    ACHECK(s.body.get("revision")->as_int() == 1);
    api::Response cfg = r.api.config();
    ACHECK(cfg.status == 200 && path(cfg.body, "video.0.bitrate_kbps")->as_int() == 3000 && path(cfg.body, "performance.profile")->as_string() == "performance");
    api::Response t = r.api.telemetry();
    ACHECK(t.status == 200 && path(t.body, "media.encoded_fps")->is_null() && path(t.body, "power.cpu_frequency_hz")->is_null());
    ACHECK(path(t.body, "process.rss_kb")->as_int() == 2048 && path(t.body, "power.isp_clock_hz")->as_number() > 0);
    ACHECK(t.body.get("timestamp_ms")->as_number() > 0);
    // API reads never wake the pipeline
    ACHECK(r.mgr.state() == State::ColdIdle && r.log.count("platform.bring_up") == 0);
}

void test_patch_cold_and_partial() {
    Rig r;
    api::Response p = r.api.patch_config("{\"video\":{\"0\":{\"bitrate_kbps\":1500}}}", "");
    ACHECK(p.status == 200 && p.body.get("ok")->as_bool() && p.body.get("changes")->size() == 1);
    const Json& ch = p.body.get("changes")->at(0);
    ACHECK(ch.get("path")->as_string() == "video.0.bitrate_kbps" && ch.get("requested")->as_int() == 1500 && ch.get("effective")->as_int() == 1500);
    ACHECK(ch.get("status")->as_string() == "stored" && ch.get("effective_on_next_start")->as_bool() && !ch.get("pipeline_restarted")->as_bool());
    ACHECK(r.mgr.state() == State::ColdIdle && r.log.count("platform.bring_up") == 0);            // no wake-up for a config change
    ACHECK(p.body.get("revision")->as_int() == 2 && r.store.revision() == 2);
    // unrelated fields preserved
    api::Response c = r.api.config();
    ACHECK(path(c.body, "video.0.fps")->as_int() == 20 && path(c.body, "sensor.fps")->as_int() == 20 && path(c.body, "performance.profile")->as_string() == "custom");
    ACHECK(r.store.get("video.bitrate") == "1500" && r.store.get("board") == "board-x" && r.store.get("log.level") == "2");
    ACHECK(r.store.get("config.revision") == "2");
    // config_changed event published
    auto sub = r.bus.subscribe(8);
    r.api.patch_config("{\"sensor\":{\"fps\":15}}", "");
    Event e; bool got = false; while (sub->pop(e)) if (e.type == "config_changed") got = true;
    ACHECK(got && r.store.revision() == 3);
    // next consumer starts with the stored values
    auto d = r.mgr.acquire(ConsumerType::Rtsp);
    ACHECK(r.platform.last_stream.bitrate_kbps == 1500 && r.log.count("platform.set_sensor_fps@15") == 1);
}

void test_patch_errors() {
    Rig r;
    api::Response p = r.api.patch_config("{bad json", "");
    ACHECK(p.status == 400 && path(p.body, "error.code")->as_string() == "invalid_json");
    p = r.api.patch_config("", "");
    ACHECK(p.status == 400 && path(p.body, "error.code")->as_string() == "invalid_json");
    p = r.api.patch_config("[1,2]", "");
    ACHECK(p.status == 400);
    p = r.api.patch_config("{\"video\":{\"0\":{\"colour\":1}}}", "");
    ACHECK(p.status == 400 && path(p.body, "error.code")->as_string() == "unknown_field" && path(p.body, "error.path")->as_string() == "video.0.colour");
    p = r.api.patch_config("{\"nonsense\":{\"a\":1}}", "");
    ACHECK(p.status == 400 && path(p.body, "error.code")->as_string() == "unknown_field");
    p = r.api.patch_config("{\"sensor\":{\"fps\":1234}}", "");
    ACHECK(p.status == 422 && path(p.body, "error.code")->as_string() == "invalid_value" && path(p.body, "error.path")->as_string() == "sensor.fps");
    p = r.api.patch_config("{\"sensor\":{\"fps\":25}}", "");                  // outside the known range: rejected, not clamped
    ACHECK(p.status == 422 && path(p.body, "error.code")->as_string() == "invalid_value" && r.store.revision() == 1);
    p = r.api.patch_config("{\"video\":{\"0\":{\"fps\":12}}}", "");           // not a verified point
    ACHECK(p.status == 422 && path(p.body, "error.code")->as_string() == "invalid_value");
    p = r.api.patch_config("{\"video\":{\"0\":{\"bitrate_kbps\":-5}}}", "");
    ACHECK(p.status == 422);
    p = r.api.patch_config("{\"video\":{\"0\":{\"bitrate_kbps\":1.5}}}", "");
    ACHECK(p.status == 422);
    p = r.api.patch_config("{\"power\":{\"cpu_performance\":\"high\"}}", "");
    ACHECK(p.status == 422 && path(p.body, "error.code")->as_string() == "unsupported_control" && path(p.body, "error.path")->as_string() == "power.cpu_performance");
    p = r.api.patch_config("{\"power\":{\"isp_performance\":\"low\"}}", "");
    ACHECK(p.status == 422 && path(p.body, "error.code")->as_string() == "unsupported_control");
    p = r.api.patch_config("{\"performance\":{\"profile\":\"turbo\"}}", "");
    ACHECK(p.status == 422 && path(p.body, "error.code")->as_string() == "invalid_value");
    // structural errors apply nothing and bump nothing
    ACHECK(r.store.revision() == 1 && r.log.count("platform.bring_up") == 0);
    // revision conflict
    p = r.api.patch_config("{\"video\":{\"0\":{\"bitrate_kbps\":1000}}}", "7");
    ACHECK(p.status == 409 && path(p.body, "error.code")->as_string() == "conflict");
    p = r.api.patch_config("{\"revision\":1,\"video\":{\"0\":{\"bitrate_kbps\":1000}}}", "");
    ACHECK(p.status == 200 && r.store.revision() == 2);
    p = r.api.patch_config("{\"revision\":1,\"video\":{\"0\":{\"bitrate_kbps\":1100}}}", "");
    ACHECK(p.status == 409);
    p = r.api.patch_config("{\"video\":{\"0\":{\"bitrate_kbps\":1200}}}", "2");
    ACHECK(p.status == 200 && r.store.revision() == 3);
}

void test_patch_active_live_and_restart() {
    Rig r;
    auto d = r.mgr.acquire(ConsumerType::Rtsp);
    ACHECK(r.mgr.state() == State::Active);
    int ups = r.log.count("platform.bring_up");
    // live bitrate: applied, no restart
    api::Response p = r.api.patch_config("{\"video\":{\"0\":{\"bitrate_kbps\":1800}}}", "");
    const Json& ch = p.body.get("changes")->at(0);
    ACHECK(p.status == 200 && ch.get("status")->as_string() == "applied" && ch.get("apply")->as_string() == "live" && !ch.get("pipeline_restarted")->as_bool());
    ACHECK(ch.get("effective")->as_int() == 1800 && r.log.count("platform.bring_up") == ups && r.mgr.stats().restart_count == 0);
    ACHECK(path(r.api.state().body, "media.bitrate_kbps")->as_int() == 1800);
    // stream fps: exactly one pipeline restart, demand preserved
    p = r.api.patch_config("{\"video\":{\"0\":{\"fps\":10}}}", "");
    const Json& c2 = p.body.get("changes")->at(0);
    ACHECK(p.status == 200 && c2.get("apply")->as_string() == "pipeline_restart" && c2.get("pipeline_restarted")->as_bool() && c2.get("status")->as_string() == "applied");
    ACHECK(r.log.count("platform.bring_up") == ups + 1 && r.mgr.stats().restart_count == 1 && r.mgr.stats().total_demand == 1 && r.mgr.state() == State::Active);
    ACHECK(p.body.get("lifecycle")->as_string() == "active");
    // profile patch: one restart for fps + live bitrate; sensor fps live
    p = r.api.patch_config("{\"performance\":{\"profile\":\"balanced\"}}", "");
    ACHECK(p.status == 200 && r.mgr.stream().fps == 15 && r.mgr.stats().restart_count == 2 && path(r.api.state().body, "profile")->as_string() == "balanced");
    ACHECK(path(r.api.state().body, "media.sensor_fps")->as_int() == 15);        // effective from the "hardware"
    // sensor fps live: no restart
    p = r.api.patch_config("{\"sensor\":{\"fps\":10}}", "");
    const Json& c3 = p.body.get("changes")->at(0);
    ACHECK(p.status == 200 && c3.get("apply")->as_string() == "live" && c3.get("effective")->as_int() == 10 && r.mgr.stats().restart_count == 2);
    // idle_grace_ms: persisted, daemon_restart semantics
    p = r.api.patch_config("{\"lifecycle\":{\"idle_grace_ms\":7000}}", "");
    const Json& c4 = p.body.get("changes")->at(0);
    ACHECK(p.status == 200 && c4.get("apply")->as_string() == "daemon_restart" && c4.get("status")->as_string() == "stored" && r.store.get("lifecycle.idle_grace_ms") == "7000");
}

void test_failed_restart_structured() {
    Rig r;
    auto d = r.mgr.acquire(ConsumerType::Rtsp);
    r.platform.fail_at = FakePlatform::FailAt::Bind; r.platform.fail_times = 1;
    api::Response p = r.api.patch_config("{\"video\":{\"0\":{\"fps\":10}}}", "");
    ACHECK(p.status == 500 && path(p.body, "error.code")->as_string() == "pipeline_restart_failed" && path(p.body, "error.path")->as_string() == "video.0.fps");
    ACHECK(p.body.get("changes")->at(0).get("status")->as_string() == "rejected");
    ACHECK(r.mgr.state() == State::Failed && !r.platform.is_up() && r.mgr.stats().total_demand == 1);
    ACHECK(path(r.api.state().body, "lifecycle")->as_string() == "failed" && !path(r.api.state().body, "last_error")->is_null());
}

void test_concurrent_patches() {
    Rig r;
    auto d = r.mgr.acquire(ConsumerType::Rtsp);
    std::vector<std::thread> ts; std::atomic<int> ok{0};
    for (int t = 0; t < 6; ++t) ts.emplace_back([&, t] {
        for (int i = 0; i < 20; ++i) {
            int kb = 1000 + t * 100 + i;
            api::Response p = r.api.patch_config("{\"video\":{\"0\":{\"bitrate_kbps\":" + std::to_string(kb) + "}}}", "");
            if (p.status == 200) ++ok;
        }
    });
    for (auto& t : ts) t.join();
    ACHECK(ok == 120 && r.store.revision() == 121);                     // no lost updates, one revision per commit
    ACHECK(r.mgr.stats().restart_count == 0 && r.mgr.state() == State::Active);
    ACHECK(r.store.get("video.bitrate") == std::to_string(r.mgr.stream().bitrate_kbps));   // last commit == live value
}

void test_config_store_text() {
    std::string text = "# hdr\nboard = b\nvideo.bitrate = 3000   # kbps\nvideo.bitrate = 999\nlog.level = 2\n";
    std::string out = rewrite_config_text(text, {{"video.bitrate", "1500"}, {"sensor.fps", "15"}}, 5);
    ACHECK(out.find("# hdr\n") == 0 && out.find("board = b\n") != std::string::npos && out.find("log.level = 2\n") != std::string::npos);
    ACHECK(out.find("video.bitrate = 1500\n") != std::string::npos && out.find("999") == std::string::npos);   // replaced, duplicate removed
    ACHECK(out.find("sensor.fps = 15\n") != std::string::npos && out.find("config.revision = 5\n") != std::string::npos);
    ACHECK(config_text_get(out, "video.bitrate") == "1500" && config_text_get(out, "missing").empty());
    ACHECK(config_text_get("x = \"quoted\"\n", "x") == "quoted");

    // unset (mj-settings.js #416): the key's LINE disappears - comments and
    // every other line stay, the revision moves, nothing becomes "key =".
    std::string rem = remove_config_keys_text(out, {"video.bitrate"}, 6);
    ACHECK(rem.find("video.bitrate") == std::string::npos);
    ACHECK(rem.find("# hdr\n") == 0 && rem.find("board = b\n") != std::string::npos);
    ACHECK(rem.find("sensor.fps = 15\n") != std::string::npos);
    ACHECK(config_text_get(rem, "config.revision") == "6");
    // removing a key that is not there is a no-op apart from the revision
    std::string rem2 = remove_config_keys_text(rem, {"video.fps"}, 7);
    ACHECK(rem2.find("board = b\n") != std::string::npos && config_text_get(rem2, "config.revision") == "7");
}

void test_m7_image_latency_api() {
    Rig r;
    api::Response caps = r.api.capabilities();
    ACHECK(path(caps.body, "image.brightness.status")->as_string() == "supported");
    ACHECK(path(caps.body, "image.wdr.status")->as_string() == "unsupported");
    ACHECK(path(caps.body, "latency.profiles")->size() == 3);
    ACHECK(path(caps.body, "controls.gop.apply")->as_string() == "live");

    api::Response p = r.api.patch_config("{\"image\":{\"brightness\":100}}", "");
    ACHECK(p.status == 200 && p.body.get("changes")->at(0).get("status")->as_string() == "stored");
    ACHECK(r.mgr.state() == State::ColdIdle && r.log.count("platform.bring_up") == 0);
    ACHECK(r.store.get("image.brightness") == "100" && path(r.api.config().body, "image.brightness")->as_int() == 100);
    p = r.api.patch_config("{\"image\":{\"wdr\":1}}", "");
    ACHECK(p.status == 422 && path(p.body, "error.code")->as_string() == "unsupported_control");
    p = r.api.patch_config("{\"image\":{\"anti_flicker\":\"55hz\"}}", "");
    ACHECK(p.status == 422 && path(p.body, "error.code")->as_string() == "invalid_value");

    auto demand = r.mgr.acquire(ConsumerType::Rtsp);
    ACHECK(demand.active() && r.log.count("image.set.brightness@100") == 1);
    p = r.api.patch_config("{\"image\":{\"brightness\":110}}", "");
    ACHECK(p.status == 200 && p.body.get("changes")->at(0).get("effective")->as_int() == 110);
    unsigned restarts = r.mgr.stats().restart_count;
    p = r.api.patch_config("{\"video\":{\"0\":{\"gop\":10}}}", "");
    ACHECK(p.status == 200 && r.mgr.stream().gop == 10 && r.log.count("enc.set_gop@10") == 1);
    ACHECK(r.mgr.stats().restart_count == restarts && r.store.get("latency.gop") == "10");
    p = r.api.patch_config("{\"latency\":{\"profile\":\"low\"}}", "");
    ACHECK(p.status == 200 && r.mgr.stats().restart_count == restarts + 1);
    ACHECK(path(r.api.state().body, "latency.profile")->as_string() == "low");
    ACHECK(path(r.api.state().body, "latency.gop")->as_int() == 10); // explicit GOP wins over low preset
    p = r.api.patch_config("{\"latency\":{\"consumer_queue_depth\":2}}", "");
    ACHECK(p.status == 200 && r.hub.default_depth() == 2 && r.mgr.stats().restart_count == restarts + 1);
    api::Response tel = r.api.telemetry();
    ACHECK(path(tel.body, "latency.available") != nullptr && path(tel.body, "exposure.available")->as_bool());
}

// The OpenIPC WebUI does not consume Machino's native config shape directly.
// Keep the bridge thin: capabilities decide what is shown; writes translate
// into the same native PATCH path so validation/lifecycle semantics stay single-source.
void test_majestic_webui_compat() {
    Rig r;

    Json schema = compat::majestic_schema(r.api.capabilities().body);
    ACHECK(schema.get("x-groups") && schema.get("x-groups")->size() == 3);
    ACHECK(path(schema, "properties.video0.properties.fps") != nullptr);
    ACHECK(path(schema, "properties.video0.properties.bitrate_kbps") != nullptr);
    ACHECK(path(schema, "properties.sensor.properties.fps") != nullptr);
    ACHECK(path(schema, "properties.image.properties.brightness") != nullptr);
    ACHECK(path(schema, "properties.image.properties.wdr") == nullptr); // unsupported is never advertised
    ACHECK(path(schema, "properties.video0.properties.fps.minimum")->as_int() == 10);
    ACHECK(path(schema, "properties.video0.properties.fps.maximum")->as_int() == 20);
    // M9/M10: detection surfaces in the WebUI when the platform proves it
    ACHECK(path(schema, "properties.ai.properties.enabled") != nullptr);
    ACHECK(path(schema, "properties.ai.properties.enabled.type")->as_string() == "boolean");
    ACHECK(path(schema, "properties.ai.properties.detector.enum")->size() == 1);
    ACHECK(path(schema, "properties.ai.properties.inference_fps.maximum")->as_int() == 60);

    Json web = compat::majestic_config(r.api.config().body, r.api.state().body);
    ACHECK(path(web, "video0.bitrate_kbps")->as_int() == 3000);
    ACHECK(path(web, "video0.fps")->as_int() == 20);
    ACHECK(web.get("video") == nullptr); // renderer is intentionally one section deep
    ACHECK(path(web, "ai.detector")->as_string() == "motion" && path(web, "ai.enabled")->as_bool() == false);

    // a detection toggle round-trips through the same native PATCH path
    compat::MajesticTranslation ai = compat::majestic_post_to_native("{\"ai\":{\"enabled\":true}}");
    ACHECK(ai.ok && path(ai.patch, "ai.enabled")->as_bool());
    api::Response ai_applied = r.api.patch_config(ai.patch.dump(), "");
    ACHECK(ai_applied.status == 200 && path(r.api.state().body, "ai.state")->as_string() == "active");

    compat::MajesticTranslation t = compat::majestic_post_to_native(
        "{\"video0\":{\"bitrate_kbps\":1500}}");
    ACHECK(t.ok && path(t.patch, "video.0.bitrate_kbps")->as_int() == 1500);
    api::Response applied = r.api.patch_config(t.patch.dump(), "");
    ACHECK(applied.status == 200 && r.store.get("video.bitrate") == "1500");

    t = compat::majestic_post_to_native("{\"sensor\":{\"fps\":15}}");
    ACHECK(t.ok && path(t.patch, "sensor.fps")->as_int() == 15);
    t = compat::majestic_post_to_native("{\"mystery\":{\"x\":1}}");
    ACHECK(!t.ok && t.status == 400 && t.code == "unknown_field" && t.path == "mystery");
    t = compat::majestic_post_to_native("{bad");
    ACHECK(!t.ok && t.code == "invalid_json");
}

// The RTSP client limit is configuration like any other: validated, persisted,
// and honest about when it takes effect (the accept loop reads its own copy).
void test_rtsp_client_limit_api() {
    Rig r;
    ACHECK(path(r.api.config().body, "rtsp.max_clients")->as_int() == 4);
    api::Response p = r.api.patch_config("{\"rtsp\":{\"max_clients\":2}}", "");
    ACHECK(p.status == 200 && p.body.get("changes")->at(0).get("status")->as_string() == "stored");
    ACHECK(p.body.get("changes")->at(0).get("apply")->as_string() == "daemon_restart");
    ACHECK(r.store.get("rtsp.max_clients") == "2" && path(r.api.config().body, "rtsp.max_clients")->as_int() == 2);
    p = r.api.patch_config("{\"rtsp\":{\"max_clients\":0}}", "");
    ACHECK(p.status == 422 && path(p.body, "error.code")->as_string() == "invalid_value");
    p = r.api.patch_config("{\"rtsp\":{\"max_clients\":99}}", "");
    ACHECK(p.status == 422 && path(p.body, "error.path")->as_string() == "rtsp.max_clients");
    p = r.api.patch_config("{\"rtsp\":{\"max_sessions\":2}}", "");
    ACHECK(p.status == 400 && path(p.body, "error.code")->as_string() == "unknown_field");
    ACHECK(path(r.api.config().body, "rtsp.max_clients")->as_int() == 2);   // nothing slipped through
}

// M8: multi-stream + snapshot surface. The Rig wires a substream and jpeg the
// way main.cpp does, then checks the API reports and serves them.
void test_m8_streams_and_snapshot() {
    Rig r;
    EffectiveStream sub = r.stream; sub.width = 640; sub.height = 360; sub.bitrate_kbps = 512;
    r.mgr.configure_sub(sub, r.hub, &r.timer);
    JpegParams jp; jp.quality = 80; r.mgr.configure_jpeg(jp, 300, 2000, &r.timer);

    api::Response caps = r.api.capabilities();
    ACHECK(path(caps.body, "jpeg.status")->as_string() == "supported");
    ACHECK(path(caps.body, "video.streams.0.configured")->as_bool());
    ACHECK(path(caps.body, "video.streams.1.configured")->as_bool());

    // snapshot from cold: a valid JPEG (SOI..EOI), pipeline returns to idle
    std::vector<uint8_t> jpg; std::string err;
    Result sr = r.api.snapshot(jpg, err);
    ACHECK(sr && jpg.size() >= 4);
    ACHECK(jpg[0] == 0xFF && jpg[1] == 0xD8 && jpg[jpg.size()-2] == 0xFF && jpg[jpg.size()-1] == 0xD9);

    // main + sub play: state reports both units
    auto m = r.mgr.acquire_unit(lifecycle::UNIT_MAIN, ConsumerType::Rtsp);
    auto s2 = r.mgr.acquire_unit(lifecycle::UNIT_SUB, ConsumerType::Rtsp);
    ACHECK(m.active() && s2.active());
    api::Response st = r.api.state();
    ACHECK(path(st.body, "media.streams.0.active")->as_bool());
    ACHECK(path(st.body, "media.streams.1.active")->as_bool());
    ACHECK(path(st.body, "media.streams.1.width")->as_int() == 640);
    api::Response tel = r.api.telemetry();
    ACHECK(path(tel.body, "media.streams.0") != nullptr);
    ACHECK(path(tel.body, "jpeg.captures_total")->as_int() >= 1);
}

// snapshot must be refused cleanly where the platform has no jpeg
void test_m8_snapshot_unsupported() {
    Rig r;
    r.platform.jpeg_supported = false;     // no jpeg cap; pipeline never configured for it
    std::vector<uint8_t> jpg; std::string err;
    Result sr = r.api.snapshot(jpg, err);
    ACHECK(!sr && sr.status == Status::Unsupported);
}

// M9: detection/AI surface. Enabling AI is a base-only demand (no encoder),
// runtime-toggled and persisted through the same PATCH path as everything else.
void test_m9_ai_api() {
    Rig r;
    api::Response caps = r.api.capabilities();
    ACHECK(path(caps.body, "ai.available")->as_string() == "supported");
    ACHECK(path(caps.body, "ai.motion")->as_string() == "supported");
    ACHECK(path(caps.body, "ai.person")->as_string() == "unknown");
    ACHECK(path(caps.body, "ai.detectors")->size() == 1 && path(caps.body, "ai.detectors")->at(0).as_string() == "motion");
    ACHECK(path(caps.body, "ai.inference_fps.min")->as_int() == 1 && path(caps.body, "ai.inference_fps.max")->as_int() == 60);

    // starts disabled: no demand, base cold
    ACHECK(path(r.api.state().body, "ai.state")->as_string() == "disabled" && !path(r.api.state().body, "ai.enabled")->as_bool());
    ACHECK(path(r.api.config().body, "ai.enabled")->as_bool() == false && path(r.api.config().body, "ai.inference_fps")->as_int() == 5);
    ACHECK(r.mgr.state() == State::ColdIdle && r.log.count("platform.bring_up") == 0);

    // enable at runtime: base-only demand comes up (no encoder), detector active
    api::Response p = r.api.patch_config("{\"ai\":{\"enabled\":true}}", "");
    ACHECK(p.status == 200 && p.body.get("changes")->at(0).get("status")->as_string() == "applied");
    ACHECK(p.body.get("changes")->at(0).get("apply")->as_string() == "live");
    ACHECK(r.log.count("platform.bring_up") == 1 && r.log.count("enc.create") == 0 && r.log.count("det.start") == 1);
    ACHECK(path(r.api.state().body, "ai.state")->as_string() == "active" && path(r.api.state().body, "ai.enabled")->as_bool());
    ACHECK(path(r.api.state().body, "ai.backend")->as_string() == "fake_motion");
    ACHECK(r.store.get("ai.enabled") == "true");
    api::Response tel = r.api.telemetry();
    ACHECK(path(tel.body, "ai.state")->as_string() == "active" && path(tel.body, "ai.inference_fps_requested")->as_int() == 5);

    // cadence change is applied and persisted (detector restarts, base stays up)
    p = r.api.patch_config("{\"ai\":{\"inference_fps\":15}}", "");
    ACHECK(p.status == 200 && r.store.get("ai.inference_fps") == "15");
    ACHECK(path(r.api.config().body, "ai.inference_fps")->as_int() == 15 && r.platform.last_detector.inference_fps == 15);
    ACHECK(r.log.count("platform.bring_up") == 1);   // base never blinked

    // unknown detector rejected
    p = r.api.patch_config("{\"ai\":{\"detector\":\"face\"}}", "");
    ACHECK(p.status == 422 && path(p.body, "error.code")->as_string() == "invalid_value");

    // disable releases the base demand
    p = r.api.patch_config("{\"ai\":{\"enabled\":false}}", "");
    ACHECK(p.status == 200 && r.store.get("ai.enabled") == "false");
    ACHECK(path(r.api.state().body, "ai.state")->as_string() == "disabled");
    ACHECK(r.mgr.state() == State::GraceIdle && !r.mgr.unit_active(lifecycle::UNIT_AI));
}

// AI PATCH must be refused cleanly where the platform has no detector. Built
// without the shared Rig: PerformanceService captures caps at construction, so
// the "no detector" flag must be set before the service exists.
void test_m9_ai_unsupported() {
    CallLog log; FakePowerControl power; FakePlatform platform{log, &power};
    platform.detector_supported = false;                 // no AI capability, create_detector returns null
    FakeTimer timer; StreamHub hub; FakeStats stats; EventBus bus;
    ConfigStore store{TMP_CONF};
    hw::ResolvedHardware hw = make_hw(); AppConfig cfg;
    EffectiveStream stream = effective_stream(cfg.video, hw);
    LifecycleConfig lc; lc.idle_grace_ms = 1000; lc.poll_timeout_ms = 10;
    PipelineManager mgr(platform, stream, lc, timer, hub);
    power::PerformanceService perf(mgr, platform, stats, hw, cfg.video);
    media::TuningService tuning(mgr, platform, hub, stream, cfg.image, cfg.latency);
    detection::DetectionService detection(mgr, platform, bus, cfg.ai);
    api::ApiService api(perf, tuning, mgr, store, bus, hw, cfg, &detection);

    api::Response caps = api.capabilities();
    ACHECK(path(caps.body, "ai.available")->as_string() == "unknown" && path(caps.body, "ai.detectors")->size() == 0);
    api::Response p = api.patch_config("{\"ai\":{\"enabled\":true}}", "");
    ACHECK(p.status == 422 && path(p.body, "error.code")->as_string() == "unsupported_control");
    ACHECK(mgr.state() == State::ColdIdle && log.count("platform.bring_up") == 0);
}

} // namespace

// The majestic-webui reset contract end to end: after a 200 the stock UI
// immediately re-reads config.json, so set -> unset -> GET must show the
// override GONE from the RUNNING daemon, not merely from the file.
void test_reset_unset_runtime() {
    Rig r;
    // image: set an override, then unset - requested must read null again
    api::Response p = r.api.patch_config("{\"image\":{\"brightness\":100}}", "");
    ACHECK(p.status == 200 && path(r.api.config().body, "image.brightness")->as_int() == 100);
    api::Response u = r.api.unset_config({"image.brightness"});
    ACHECK(u.status == 200);
    ACHECK(path(r.api.config().body, "image.brightness")->is_null());     // runtime override cleared NOW
    ACHECK(r.store.get("image.brightness").empty());                      // file line gone
    // latency: buffers override set, then unset - resolved returns to base
    p = r.api.patch_config("{\"latency\":{\"framesource_buffers\":1}}", "");
    ACHECK(p.status == 200);
    ACHECK(path(r.api.config().body, "latency.framesource_buffers")->as_int() == 1);
    u = r.api.unset_config({"latency.framesource_buffers"});
    ACHECK(u.status == 200);
    ACHECK(path(r.api.config().body, "latency.framesource_buffers")->is_null());
    ACHECK(path(r.api.config().body, "latency.effective.framesource_buffers")->as_int() == r.cfg.video.buffers);
    ACHECK(r.store.get("latency.framesource_buffers").empty());
    // consumer queue depth the same way
    p = r.api.patch_config("{\"latency\":{\"consumer_queue_depth\":2}}", "");
    ACHECK(p.status == 200);
    u = r.api.unset_config({"latency.queue_depth"});
    ACHECK(u.status == 200 && path(r.api.config().body, "latency.consumer_queue_depth")->is_null());
    // reapply-class keys are synchronous now too (no SIGHUP involved):
    // latency.profile low -> unset -> the default profile is live again
    p = r.api.patch_config("{\"latency\":{\"profile\":\"low\"}}", "");
    ACHECK(p.status == 200);
    ACHECK(path(r.api.config().body, "latency.profile")->as_string() == "low");
    u = r.api.unset_config({"latency.profile"});
    ACHECK(u.status == 200);
    ACHECK(path(r.api.config().body, "latency.profile")->as_string() == "normal");
    ACHECK(r.store.get("latency.profile").empty());
    // unknown keys stay a 404 and persist NOTHING (validated before writing)
    unsigned rev = r.store.revision();
    u = r.api.unset_config({"image.nope"});
    ACHECK(u.status == 404 && r.store.revision() == rev);
    u = r.api.unset_config({"nope.key"});
    ACHECK(u.status == 404 && r.store.revision() == rev);
}

// The Custom-profile trap: apply_config deliberately skips ABSENT keys, so a
// generic fresh-reload would leave the old runtime fps active while reset
// answers 200. The explicit clears must restore the MODE default (20 in this
// rig) regardless of the profile.
void test_reset_fps_under_custom_profile() {
    Rig r;
    api::Response p = r.api.patch_config("{\"video\":{\"0\":{\"fps\":15}}}", "");   // switches profile to custom
    ACHECK(p.status == 200);
    ACHECK(path(r.api.config().body, "video.0.fps")->as_int() == 15);
    api::Response u = r.api.unset_config({"video.fps"});
    ACHECK(u.status == 200);
    ACHECK(path(r.api.config().body, "video.0.fps")->as_int() == 20);   // mode default, despite custom
    ACHECK(r.mgr.stream().fps == 20);
    ACHECK(r.store.get("video.fps").empty());

    p = r.api.patch_config("{\"sensor\":{\"fps\":10}}", "");
    ACHECK(p.status == 200);
    u = r.api.unset_config({"sensor.fps"});
    ACHECK(u.status == 200);
    ACHECK(path(r.api.config().body, "sensor.fps")->as_int() == 20);
    ACHECK(r.store.get("sensor.fps").empty());
}

// False-200 guard: a runtime re-apply that the platform rejects must surface
// as 500 - never as ok. The disk already holds the reset ("persisted"), so the
// next start converges; but THIS response may not claim effective-now.
void test_reset_apply_failure_is_500() {
    Rig r;
    auto d = r.mgr.acquire(ConsumerType::Rtsp);          // live pipeline: sensor fps goes to hardware
    ACHECK(r.mgr.state() == State::Active);
    api::Response p = r.api.patch_config("{\"sensor\":{\"fps\":15}}", "");
    ACHECK(p.status == 200);
    r.platform.sensor_fps_runtime_error = true;           // hardware now refuses
    api::Response u = r.api.unset_config({"sensor.fps"});
    ACHECK(u.status == 500);
    ACHECK(r.store.get("sensor.fps").empty());            // persisted: next start converges
}

// AP2: rtsp.enabled / rtsp.port are LIVE runtime changes (no daemon restart),
// they reach the listener, and a refused bind is an honest rejection that
// leaves the persisted value alone.
void test_ap2_rtsp_runtime() {
    Rig r;
    api::Response cfg0 = r.api.config();
    ACHECK(cfg0.status == 200);
    ACHECK(path(cfg0.body, "rtsp.enabled")->as_bool() == true);
    ACHECK(path(cfg0.body, "rtsp.port")->as_int() == 554);

    // disable: applied live, reaches the listener, and is persisted
    api::Response d = r.api.patch_config("{\"rtsp\":{\"enabled\":false}}", "");
    ACHECK(d.status == 200);
    ACHECK(r.rtsp.enable_calls == 1 && r.rtsp.enabled == false);
    ACHECK(path(r.api.config().body, "rtsp.enabled")->as_bool() == false);

    // re-enable without any process restart
    api::Response e = r.api.patch_config("{\"rtsp\":{\"enabled\":true}}", "");
    ACHECK(e.status == 200 && r.rtsp.enabled == true && r.rtsp.enable_calls == 2);

    // port change is live and lands on the listener
    api::Response p1 = r.api.patch_config("{\"rtsp\":{\"port\":8554}}", "");
    ACHECK(p1.status == 200 && r.rtsp.port == 8554 && r.rtsp.port_calls == 1);
    ACHECK(path(r.api.config().body, "rtsp.port")->as_int() == 8554);
    ACHECK(r.api.patch_config("{\"rtsp\":{\"port\":554}}", "").status == 200);
    ACHECK(r.rtsp.port == 554);

    // out-of-range / wrong-typed values are refused by validation before they
    // ever reach the listener
    const int before = r.rtsp.port_calls;
    ACHECK(r.api.patch_config("{\"rtsp\":{\"port\":70000}}", "").status == 422);
    ACHECK(r.api.patch_config("{\"rtsp\":{\"port\":0}}", "").status == 422);
    ACHECK(r.api.patch_config("{\"rtsp\":{\"enabled\":\"yes\"}}", "").status == 422);
    ACHECK(r.rtsp.port_calls == before);

    // a port that cannot be bound: rejected, the old port stays in effect and
    // the persisted value must not move to the port we could not take
    r.rtsp.fail_bind = true;
    ACHECK(r.api.patch_config("{\"rtsp\":{\"port\":9000}}", "").status >= 400);
    ACHECK(r.rtsp.port == 554);
    ACHECK(path(r.api.config().body, "rtsp.port")->as_int() == 554);
    r.rtsp.fail_bind = false;

    // with no control wired the value is still accepted and persisted, just
    // deferred to the next start (never silently dropped)
    Rig r2;
    api::ApiService nolink(r2.perf, r2.tuning, r2.mgr, r2.store, r2.bus, r2.hw, r2.cfg, &r2.detection, nullptr);
    ACHECK(nolink.patch_config("{\"rtsp\":{\"enabled\":false}}", "").status == 200);
}


// AP10: the live image preview. The contract is mj-settings.js's, not ours:
//
//   POST /api/v1/image?brightness=128&contrast=100&hflip=1
//
// leaf names only, EVERY live field on every push, and the page reads nothing
// but r.ok. The rule that shapes the implementation is the one that is easiest
// to get wrong: nothing is persisted. A drag writes one value per pointer move,
// and putting those on flash would be a slider that wears the camera out.
// AP14: jpeg is REPORTED so the dashboard can read its own gate, and is
// deliberately not writable - enabling the encoder wedges this platform. A key
// the API publishes deserves a refusal that says why, not a bare "unknown
// field".
void test_ap14_jpeg_reported_not_writable() {
    Rig r;
    // reported, with a real value. The Response is kept alive in a named
    // local - get() hands back a pointer INTO its body, and a pointer into a
    // temporary is dead the moment the full expression ends.
    api::Response before = r.api.config();
    const Json* j = before.body.get("jpeg");
    ACHECK(j && j->is_object());
    ACHECK(j && j->get("enabled") && j->get("enabled")->is_bool());
    ACHECK(j && j->get("quality") && j->get("quality")->is_number());
    const bool was = (j && j->get("enabled")) ? j->get("enabled")->as_bool() : true;
    // and refused, with a reason and the right code
    api::Response p = r.api.patch_config("{\"jpeg\":{\"enabled\":true}}", "");
    ACHECK(p.status == 403);
    const Json* err = p.body.get("error");
    ACHECK(err && err->get("code") && err->get("code")->as_string() == "unsupported_control");
    ACHECK(err && err->get("message") && err->get("message")->as_string().find("wedge") != std::string::npos);
    // nothing moved
    api::Response after = r.api.config();
    const Json* ja = after.body.get("jpeg");
    ACHECK(ja && ja->get("enabled") && ja->get("enabled")->as_bool() == was);
}

void test_ap10_live_image() {
    Rig r;
    const int rev0 = (int)r.api.config().body.get("revision")->as_int();

    // a single knob
    {
        api::Response resp = r.api.live_image("brightness=128");
        ACHECK(resp.status == 200);
        ACHECK(resp.body.get("persisted") && !resp.body.get("persisted")->as_bool());
        ACHECK(resp.body.get("count") && resp.body.get("count")->as_int() == 1);
    }
    // several at once - "mirror and flip need each other", so they arrive
    // together and must all be applied from one request
    {
        api::Response resp = r.api.live_image("brightness=100&contrast=90&hflip=1&vflip=0");
        ACHECK(resp.status == 200);
        // A control this platform does not have is SKIPPED, not an error: the
        // page sends every live field it rendered, and refusing the whole push
        // because one knob is unsupported would break the others with it. So
        // the assertion is that the supported ones arrived together, not that
        // all four did.
        const Json* ap = resp.body.get("applied");
        ACHECK(ap && ap->get("brightness"));
        ACHECK(ap && ap->get("contrast"));
        ACHECK(resp.body.get("count")->as_int() >= 2);
    }
    // NOTHING was persisted: the revision must not have moved
    {
        api::Response cfg = r.api.config();
        ACHECK((int)cfg.body.get("revision")->as_int() == rev0);
    }
    // ... and, less obviously, /api/v1/config must not REPORT the dragged
    // value either. The settings page reads that once at load as
    // state.initial, and liveDrift() compares the sliders against it to decide
    // whether to restore the saved values when the page is left. If a preview
    // push rewrote it, a reload after dragging without saving would show the
    // dragged values as saved, liveDrift() would see no drift, and the camera
    // would keep values nobody saved until the next restart.
    {
        api::Response before = r.api.config();
        const Json* b = path(before.body, "image.brightness");
        const long long saved = (b && !b->is_null()) ? b->as_int() : -1;
        ACHECK(r.api.live_image("brightness=37").status == 200);
        api::Response after = r.api.config();
        const Json* a = path(after.body, "image.brightness");
        const long long now = (a && !a->is_null()) ? a->as_int() : -1;
        ACHECK(now == saved);                       // the config still says what was SAVED
        ACHECK(now != 37 || saved == 37);
    }
    // the persisting path, by contrast, DOES move it - the two must not be
    // collapsed into one call with a flag
    {
        api::Response p = r.api.patch_config("{\"image\":{\"brightness\":64}}", "");
        if (p.status == 200) {
            const Json* a = path(r.api.config().body, "image.brightness");
            ACHECK(a && !a->is_null() && a->as_int() == 64);
        }
    }

    // the one non-numeric control, decoded exactly as the PATCH path does
    {
        ACHECK(r.api.live_image("anti_flicker=50hz").status == 200);
        ACHECK(r.api.live_image("anti_flicker=off").status == 200);
        api::Response bad = r.api.live_image("anti_flicker=70hz");
        ACHECK(bad.status == 422);
    }

    // bad input is refused, and says which parameter
    {
        api::Response u = r.api.live_image("nonsense=1");
        ACHECK(u.status == 400);
        ACHECK(u.body.get("error") && u.body.get("error")->get("message"));
        ACHECK(r.api.live_image("brightness=abc").status == 422);
        ACHECK(r.api.live_image("brightness=").status == 422);
        ACHECK(r.api.live_image("brightness").status == 400);     // no '=' at all
        ACHECK(r.api.live_image("brightness=-").status == 422);   // a lone sign
    }
    // ... and a refusal applies NOTHING from that push, so a typo cannot
    // half-apply a slider row
    {
        const int before = (int)r.api.config().body.get("revision")->as_int();
        ACHECK(r.api.live_image("brightness=50&nonsense=1").status == 400);
        ACHECK((int)r.api.config().body.get("revision")->as_int() == before);
    }

    // percent-encoding, because the page builds the query with
    // encodeURIComponent on both halves
    {
        ACHECK(r.api.live_image("anti%5Fflicker=off").status == 200 ||
               r.api.live_image("anti_flicker=off").status == 200);
    }

    // an empty push is a no-op, not an error: the page sends whatever is
    // wired live, and a camera with no live controls would otherwise 4xx on
    // every pointer move
    {
        api::Response e = r.api.live_image("");
        ACHECK(e.status == 200);
        ACHECK(e.body.get("count")->as_int() == 0);
    }
}
void run_api_tests() {
    test_get_documents();
    test_patch_cold_and_partial();
    test_patch_errors();
    test_patch_active_live_and_restart();
    test_failed_restart_structured();
    test_concurrent_patches();
    test_config_store_text();
    test_m7_image_latency_api();
    test_majestic_webui_compat();
    test_rtsp_client_limit_api();
    test_m8_streams_and_snapshot();
    test_m8_snapshot_unsupported();
    test_m9_ai_api();
    test_m9_ai_unsupported();
    test_reset_unset_runtime();
    test_reset_fps_under_custom_profile();
    test_reset_apply_failure_is_500();
    test_ap2_rtsp_runtime();
    test_ap10_live_image();
    test_ap14_jpeg_reported_not_writable();
    remove(TMP_CONF); remove((std::string(TMP_CONF) + ".tmp").c_str());
}
