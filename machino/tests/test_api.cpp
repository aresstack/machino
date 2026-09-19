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

struct Rig {
    CallLog log; FakePowerControl power; FakePlatform platform{log, &power}; FakeTimer timer; StreamHub hub; FakeStats stats;
    EventBus bus; ConfigStore store{TMP_CONF};
    hw::ResolvedHardware hw; AppConfig cfg; EffectiveStream stream; LifecycleConfig lc;
    PipelineManager mgr; power::PerformanceService perf; media::TuningService tuning;
    detection::DetectionService detection; api::ApiService api;
    Rig() : hw(make_hw()), stream(effective_stream(cfg.video, hw)), mgr(platform, stream, mk_lc(), timer, hub),
            perf(mgr, platform, stats, hw, cfg.video), tuning(mgr, platform, hub, stream, cfg.image, cfg.latency),
            detection(mgr, platform, bus, cfg.ai), api(perf, tuning, mgr, store, bus, hw, cfg, &detection) {
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

    Json web = compat::majestic_config(r.api.config().body, r.api.state().body);
    ACHECK(path(web, "video0.bitrate_kbps")->as_int() == 3000);
    ACHECK(path(web, "video0.fps")->as_int() == 20);
    ACHECK(web.get("video") == nullptr); // renderer is intentionally one section deep

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
    remove(TMP_CONF); remove((std::string(TMP_CONF) + ".tmp").c_str());
}
