// M10: one-way majestic.yaml -> machino migration. Proves the classification
// (mapped/converted/ignored/unsupported/invalid), the value transforms
// (GOP seconds->frames, image 0..100->0..255, resolution split, motionDetect
// -> AI), and honest failure on garbage.
#include "app/compat/majestic_migrate.hpp"
#include "app/compat/majestic_webui.hpp"
#include "core/json.hpp"

#include <cstdio>
#include <string>

using namespace machino;
using namespace machino::compat;

extern int g_fail_ext, g_pass_ext;
#define CCHECK(cond) do { if (cond) { ++g_pass_ext; } else { ++g_fail_ext; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

namespace {

const MigrationEntry* find(const MigrationResult& r, const char* key) {
    for (const auto& e : r.entries) if (e.source_key == key) return &e;
    return nullptr;
}
bool disp_is(const MigrationResult& r, const char* key, Disposition d) {
    const MigrationEntry* e = find(r, key);
    return e && e->disp == d;
}
std::string cfgval(const MigrationResult& r, const char* key) {
    for (const auto& kv : r.config) if (kv.first == key) return kv.second;
    return std::string();
}

void test_migrate_core() {
    const char* yaml =
        "system:\n"
        "  webPort: 80\n"
        "  logLevel: info\n"
        "isp:\n"
        "  sensorConfig: /etc/sensors/imx307.bin\n"
        "video0:\n"
        "  enabled: true\n"
        "  codec: h264\n"
        "  size: 1920x1080\n"
        "  fps: 25\n"
        "  bitrate: 4096   # kbit/s\n"
        "  gop: 2\n"
        "  rcMode: cbr\n"
        "audio:\n"
        "  enabled: false\n"
        "rtsp:\n"
        "  port: 554\n"
        "motionDetect:\n"
        "  enabled: true\n";
    MigrationResult r = migrate_majestic_yaml(yaml);
    CCHECK(r.ok);
    // values that carry over
    CCHECK(cfgval(r, "video.fps") == "25");
    CCHECK(cfgval(r, "video.bitrate") == "4096");                 // inline comment stripped
    CCHECK(cfgval(r, "video.width") == "1920" && cfgval(r, "video.height") == "1080");
    CCHECK(cfgval(r, "latency.gop") == "50");                     // 2 s * 25 fps
    CCHECK(cfgval(r, "video.rc_mode") == "cbr");
    CCHECK(cfgval(r, "rtsp.port") == "554");
    CCHECK(cfgval(r, "log.level") == "2");
    CCHECK(cfgval(r, "ai.enabled") == "true" && cfgval(r, "ai.detector") == "motion");
    // dispositions
    CCHECK(disp_is(r, "system.webPort", Disposition::Ignored));
    CCHECK(disp_is(r, "system.logLevel", Disposition::Converted));
    CCHECK(disp_is(r, "isp.sensorConfig", Disposition::Ignored));
    CCHECK(disp_is(r, "video0.codec", Disposition::Ignored));     // h264 is what machino streams
    CCHECK(disp_is(r, "video0.enabled", Disposition::Ignored));   // demand-driven
    CCHECK(disp_is(r, "video0.size", Disposition::Converted));
    CCHECK(disp_is(r, "video0.gop", Disposition::Converted));
    CCHECK(disp_is(r, "audio.enabled", Disposition::Unsupported));
    CCHECK(disp_is(r, "motionDetect.enabled", Disposition::Converted));
    // the conf body carries only mapped+converted keys, with a header
    std::string conf = to_machino_conf(r);
    CCHECK(conf.find("# machino.conf migrated from majestic.yaml") == 0);
    CCHECK(conf.find("video.fps = 25\n") != std::string::npos);
    CCHECK(conf.find("webPort") == std::string::npos);           // ignored keys never reach the conf
}

void test_migrate_edges() {
    // h265 unsupported; gop needs fps; disabling the main stream is unsupported
    const char* yaml =
        "video0:\n"
        "  codec: h265\n"
        "  gop: 2\n"
        "  enabled: false\n";
    MigrationResult r = migrate_majestic_yaml(yaml);
    CCHECK(disp_is(r, "video0.codec", Disposition::Unsupported));
    CCHECK(disp_is(r, "video0.gop", Disposition::Invalid));       // no fps to convert against
    CCHECK(disp_is(r, "video0.enabled", Disposition::Unsupported));

    // empty / unrecognisable documents fail honestly
    CCHECK(!migrate_majestic_yaml("   \n").ok);
    CCHECK(!migrate_majestic_yaml("# only a comment\n").ok);

    // substream, image rescale, jpeg clamp, list value
    const char* y2 =
        "video1:\n"
        "  enabled: true\n"
        "  size: 640x360\n"
        "  fps: 15\n"
        "  gop: 1\n"
        "image:\n"
        "  contrast: 50\n"
        "  mirror: true\n"
        "  rotate: 90\n"
        "jpeg:\n"
        "  quality: 100\n"
        "onvif:\n"
        "  interfaces:\n"
        "    - eth0\n";
    MigrationResult r2 = migrate_majestic_yaml(y2);
    CCHECK(cfgval(r2, "video.1.enabled") == "true");
    CCHECK(cfgval(r2, "video.1.width") == "640" && cfgval(r2, "video.1.height") == "360");
    CCHECK(cfgval(r2, "video.1.fps") == "15");
    CCHECK(cfgval(r2, "video.1.gop") == "15");                    // 1 s * 15 fps
    CCHECK(cfgval(r2, "image.contrast") == "128");               // 50 * 255 / 100, rounded
    CCHECK(cfgval(r2, "image.hflip") == "1");
    CCHECK(disp_is(r2, "image.rotate", Disposition::Unsupported));
    CCHECK(cfgval(r2, "jpeg.quality") == "99");                   // clamped to machino max
    CCHECK(disp_is(r2, "onvif.interfaces", Disposition::Unsupported));   // list recorded, not guessed
}

bool has_sub(const std::string& hay, const std::string& needle) { return hay.find(needle) != std::string::npos; }

// The majestic-webui Dashboard reads node-exporter names from /metrics; this
// pins the exact names/labels so the tiles keep populating and the
// "Camera is not responding" heartbeat (a failing /metrics poll) stays cleared.
void test_webui_metrics() {
    Json tel = Json::object();
    Json exp = Json::object();
    exp.set("luma", Json::integer(42));
    exp.set("analog_gain", Json::integer(1024));
    exp.set("digital_gain", Json::integer(64));
    exp.set("isp_digital_gain", Json::integer(16));
    exp.set("integration_time", Json::integer(1000));
    tel.set("exposure", exp);
    Json pw = Json::object(); pw.set("sensor_fps", Json::integer(20)); tel.set("power", pw);

    Json state = Json::object();
    Json media = Json::object();
    Json streams = Json::object();
    Json s0 = Json::object(); s0.set("total_bytes", Json::integer(123456)); streams.set("0", s0);
    Json s1 = Json::object(); s1.set("total_bytes", Json::integer(789));    streams.set("1", s1);
    media.set("streams", streams); state.set("media", media);

    LinuxSample lin;
    lin.now_unix = 1000; lin.have_boot = true; lin.boot_unix = 900;
    lin.have_load = true; lin.load1 = 0.5; lin.load5 = 0.4; lin.load15 = 0.3;
    lin.have_mem = true; lin.mem_total = 64 * 1024 * 1024; lin.mem_free = 8 * 1024 * 1024;
    lin.mem_avail = 40 * 1024 * 1024;
    LinuxSample::Cpu c; c.index = 0; c.user = 100; c.idle = 900; lin.cpus.push_back(c);
    LinuxSample::Net n; n.dev = "eth0"; n.rx = 5000; n.tx = 6000; lin.nets.push_back(n);

    std::string m = majestic_metrics(tel, state, lin);
    CCHECK(has_sub(m, "node_time_seconds 1000"));
    CCHECK(has_sub(m, "node_memory_MemTotal_bytes 67108864"));
    CCHECK(has_sub(m, "node_load1 0.5"));
    CCHECK(has_sub(m, "node_cpu_seconds_total{cpu=\"0\",mode=\"idle\"} 9"));
    CCHECK(has_sub(m, "node_network_receive_bytes_total{device=\"eth0\"} 5000"));
    CCHECK(has_sub(m, "node_network_transmit_bytes_total{device=\"eth0\"} 6000"));
    CCHECK(has_sub(m, "isp_avelum 42"));
    CCHECK(has_sub(m, "isp_again 1024"));
    CCHECK(has_sub(m, "isp_fps 20"));
    CCHECK(has_sub(m, "venc0_rcvd_bytes 123456"));
    CCHECK(has_sub(m, "venc1_rcvd_bytes 789"));
    // no thermal zone in the sample -> the metric is omitted, not printed as 0
    CCHECK(!has_sub(m, "node_hwmon_temp_celsius"));
}

// majestic_config must alias native width/height/bitrate_kbps into the
// size/codec/bitrate/enabled fields the Dashboard/Streams renderer reads,
// and majestic_sources must expose them as a {sources:[...]} document.
void test_webui_config_and_sources() {
    Json native = Json::object();
    Json video = Json::object();
    Json v0 = Json::object();
    v0.set("width", Json::integer(1920)); v0.set("height", Json::integer(1080));
    v0.set("fps", Json::integer(25)); v0.set("bitrate_kbps", Json::integer(4096));
    video.set("0", v0); native.set("video", video);

    Json cfg = majestic_config(native, Json::object());
    const Json* mv0 = cfg.get("video0");
    CCHECK(mv0 && mv0->is_object());
    if (mv0) {
        const Json* size = mv0->get("size");
        CCHECK(size && size->is_string() && size->as_string() == "1920x1080");
        const Json* codec = mv0->get("codec");
        CCHECK(codec && codec->is_string() && codec->as_string() == "h264");
        const Json* br = mv0->get("bitrate");
        CCHECK(br && br->is_number() && br->as_int() == 4096);
        const Json* en = mv0->get("enabled");
        CCHECK(en && en->is_bool() && en->as_bool());
    }

    // /api/v1/sources wire format is pinned by the UPSTREAM majestic-webui
    // test fixture (tests/sources.test.js, FROM_A_REAL_CAMERA): sensor source
    // camera 0 with streams[], subtype as a NAME, flowing on h264 streams.
    Json st = Json::object();
    { Json med = Json::object(); med.set("encoder_active", Json::boolean(true)); st.set("media", med); }
    Json src = majestic_sources(cfg, st);
    const Json* arr = src.get("sources");
    CCHECK(arr && arr->is_array() && arr->size() == 1);
    if (arr && arr->size() == 1) {
        const Json& sensor = arr->at(0);
        CCHECK(sensor.get("camera") && sensor.get("camera")->as_int() == 0);
        CCHECK(sensor.get("kind") && sensor.get("kind")->as_string() == "sensor");
        const Json* streams = sensor.get("streams");
        CCHECK(streams && streams->is_array() && streams->size() == 1);
        if (streams && streams->size() == 1) {
            const Json& s0 = streams->at(0);
            CCHECK(s0.get("id") && s0.get("id")->as_int() == 0);
            CCHECK(s0.get("subtype") && s0.get("subtype")->as_string() == "main");
            CCHECK(s0.get("codec") && s0.get("codec")->as_string() == "h264");
            CCHECK(s0.get("fps") && s0.get("fps")->as_int() == 25);
            CCHECK(s0.get("width") && s0.get("width")->as_int() == 1920);
            CCHECK(s0.get("height") && s0.get("height")->as_int() == 1080);
            CCHECK(s0.get("flowing") && s0.get("flowing")->as_bool());
            CCHECK(s0.get("configured") && s0.get("configured")->as_bool());
            CCHECK(s0.get("present") && s0.get("present")->as_bool());
            CCHECK(s0.get("rtsp") && s0.get("rtsp")->as_bool());
        }
    }

    // /api/v1/get: plain-text value of a dotted key, miss = false (-> 404).
    std::string val;
    CCHECK(majestic_get(cfg, "video0.fps", val) && val == "25");
    CCHECK(majestic_get(cfg, "video0.size", val) && val == "1920x1080");
    CCHECK(majestic_get(cfg, "video0.enabled", val) && val == "true");
    CCHECK(!majestic_get(cfg, "nightMode.irCutPin1", val));   // honestly absent
    CCHECK(!majestic_get(cfg, "video0.nope", val));
    CCHECK(!majestic_get(cfg, "", val));
}

// The stock WebUI sends form values as STRINGS ("Values are always sent as
// strings; the camera coerces" - upstream docs/settings-page.md). The
// translation must hand the native validator real numbers/bools.
void test_webui_post_strings_and_reset() {
    MajesticTranslation t = majestic_post_to_native(
        "{\"video0\":{\"fps\":\"20\",\"bitrate_kbps\":\"3000\",\"gop\":\"40\"},"
        "\"sensor\":{\"fps\":\"20\"},\"ai\":{\"enabled\":\"true\"}}");
    CCHECK(t.ok);
    const Json* v0 = t.patch.get("video") ? t.patch.get("video")->get("0") : nullptr;
    CCHECK(v0 && v0->get("fps") && v0->get("fps")->is_number() && v0->get("fps")->as_int() == 20);
    CCHECK(v0 && v0->get("bitrate_kbps") && v0->get("bitrate_kbps")->is_number() && v0->get("bitrate_kbps")->as_int() == 3000);
    const Json* ai = t.patch.get("ai");
    CCHECK(ai && ai->get("enabled") && ai->get("enabled")->is_bool() && ai->get("enabled")->as_bool());
    // real string fields survive untouched
    MajesticTranslation t2 = majestic_post_to_native("{\"ai\":{\"detector\":\"motion\"}}");
    CCHECK(t2.ok && t2.patch.get("ai")->get("detector")->is_string());

    // /api/v1/reset: mappable keys produce a native default patch, others 404.
    MajesticTranslation r1 = majestic_reset("video0.gop");
    CCHECK(r1.ok && r1.patch.get("video") && r1.patch.get("video")->get("0")->get("gop")->as_int() == 40);
    MajesticTranslation r2 = majestic_reset("video0.bitrate_kbps");
    CCHECK(r2.ok && r2.patch.get("video")->get("0")->get("bitrate_kbps")->as_int() == 3000);
    MajesticTranslation r3 = majestic_reset("nightMode.irCutPin1");
    CCHECK(!r3.ok && r3.status == 404);
    // CURRENT mj-settings.js contract (#416, the CODE beats the stale docs):
    // a key with no schema default is reset by REMOVING it (unset state, 200);
    // 404 means the camera has no such setting at all.
    MajesticTranslation r4 = majestic_reset("video0.fps");
    CCHECK(r4.ok && r4.unset.size() == 1 && r4.unset[0] == "video.fps");
    MajesticTranslation r6 = majestic_reset("image.brightness");
    CCHECK(r6.ok && r6.unset.size() == 1 && r6.unset[0] == "image.brightness");
    MajesticTranslation r7 = majestic_reset("image.nope");
    CCHECK(!r7.ok && r7.status == 404);
    MajesticTranslation r8 = majestic_reset("latency.consumer_queue_depth");
    CCHECK(r8.ok && r8.unset.size() == 1 && r8.unset[0] == "latency.queue_depth");
    // rtsp.max_clients is DaemonRestart-class and no longer exposed -> 404
    MajesticTranslation r5 = majestic_reset("rtsp.max_clients");
    CCHECK(!r5.ok && r5.status == 404);

    // schema: x-reload is "live" for everything exposed (Machino applies each
    // change DURING the POST - nothing is left for Apply-now); daemon_restart-
    // class fields (lifecycle, rtsp.max_clients) are not exposed at all.
    Json caps = Json::object();
    { Json pr = Json::array(); pr.push(Json::string("performance")); pr.push(Json::string("battery")); caps.set("profiles", pr); }
    Json schema = majestic_schema(caps);
    const Json* props = schema.get("properties");
    CCHECK(props);
    if (props) {
        CCHECK(!props->get("lifecycle"));
        CCHECK(!props->get("rtsp"));
        const Json* perf = props->get("performance");
        const Json* pp = perf && perf->get("properties") ? perf->get("properties")->get("profile") : nullptr;
        CCHECK(pp && pp->get("x-reload") && pp->get("x-reload")->as_string() == "live");
    }

    // nightMode: irCut "off" is upstream's "a decision, not a defect" state -
    // no findings, no invented GPIO pins, no false red IR-cut banner.
    Json mc2 = majestic_config(Json::object(), Json::object());
    const Json* nm = mc2.get("nightMode");
    CCHECK(nm && nm->get("irCut") && nm->get("irCut")->as_string() == "off");

    // AP18: not one pin key, ALL of them. These are the nightMode keys the
    // stock UI reads as a wiring fact (extracted from the pages that use them,
    // not from a doc): every one that appears makes the camera claim a pad it
    // can drive. This board has no evidence for any of them - no IR-cut, LED,
    // infrared or motor node in the device tree, no /sys/class/leds, no PWM,
    // no ADC, no exported GPIO, and the wiki-harvested pin table in upstream's
    // ircut-pads.js lists t10/t20/t21/t31/t31l/t31n but no t40 at all.
    static const char* const PIN_KEYS[] = {
        "irCutPin1", "irCutPin2", "lightSensorPin", "backlightPin", "backlightPwmChannel"
    };
    for (const char* k : PIN_KEYS) CCHECK(nm && !nm->get(k));

    // And nothing else either: exactly one key, so a later edit cannot slip a
    // capability in here without this test noticing.
    CCHECK(nm && nm->members().size() == 1);
}

// AP19: the three subsystems this camera does not offer, and the one it
// reports but cannot be written to. The distinction matters: a section the
// camera never mentions is genuinely unknown to it, and a section it publishes
// is not - answering both the same way is the AP14 defect.
void test_unoffered_subsystems() {
    // Recording, analytics and peers are never published, so a write to them
    // is an ordinary unknown field. Nothing is silently swallowed.
    for (const char* s : { "records", "analytics", "peers" }) {
        const std::string body = std::string("{\"") + s + "\":{\"enabled\":\"true\"}}";
        MajesticTranslation r = majestic_post_to_native(body);
        CCHECK(!r.ok);
        CCHECK(r.code == "unknown_field");
        CCHECK(r.path == s);
    }

    // nightMode IS published (one key), so "unknown section" would read as a
    // typo on the caller's side when the cause is the camera's hardware.
    {
        MajesticTranslation r = majestic_post_to_native("{\"nightMode\":{\"irCutPin1\":\"11\"}}");
        CCHECK(!r.ok);
        CCHECK(r.status == 403);
        CCHECK(r.code == "unsupported_control");
        CCHECK(r.path == "nightMode");
        CCHECK(r.message.find("IR-cut") != std::string::npos);
        CCHECK(r.message.find("t40") != std::string::npos);
    }

    // AP20: audio is reported too (two switches), so it gets a reason as well.
    {
        MajesticTranslation r = majestic_post_to_native("{\"audio\":{\"enabled\":\"true\"}}");
        CCHECK(!r.ok);
        CCHECK(r.status == 403);
        CCHECK(r.code == "unsupported_control");
        CCHECK(r.path == "audio");
        CCHECK(r.message.find("spk_gpio=-1") != std::string::npos);
    }

    // A MIXED body is the case worth stating rather than implying: a POST that
    // carries a good section AND a refused one must apply NEITHER. The good
    // half is translated into the local patch before the bad half is reached,
    // and only the success path ever copies it out - so the refusal carries
    // nothing. Partial application of a config POST would be the worst of the
    // three possible answers.
    {
        MajesticTranslation r = majestic_post_to_native(
            "{\"video0\":{\"bitrate\":\"2000\"},\"audio\":{\"enabled\":\"true\"}}");
        CCHECK(!r.ok);
        CCHECK(r.status == 403 && r.path == "audio");
        CCHECK(r.patch.members().empty());
    }

    // And none of them leaks into a patch: a refusal that still translated
    // something would be worse than either answer.
    for (const char* s : { "records", "analytics", "peers", "nightMode", "audio" }) {
        const std::string body = std::string("{\"") + s + "\":{\"enabled\":\"true\"}}";
        MajesticTranslation r = majestic_post_to_native(body);
        CCHECK(r.patch.members().empty());
    }
}

} // namespace

void run_compat_tests() {
    test_migrate_core();
    test_migrate_edges();
    test_webui_metrics();
    test_webui_config_and_sources();
    test_webui_post_strings_and_reset();
    test_unoffered_subsystems();
}

// AP6: the substream must be addressable through the same surfaces the main
// stream is. It was already REPORTED in the Dashboard's stream list but could
// not be CHANGED anywhere except machino.conf over SSH, which is not a drop-in.
void run_substream_schema_tests() {
    Json caps = Json::object();
    {
        Json c = Json::object();
        auto rng = [](int lo, int hi) {
            Json r = Json::object();
            r.set("status", Json::string("supported"));
            r.set("min", Json::integer(lo)); r.set("max", Json::integer(hi));
            r.set("apply", Json::string("live"));
            return r;
        };
        c.set("stream_fps", rng(1, 60));
        c.set("bitrate", rng(64, 20000));
        c.set("gop", rng(1, 1000));
        caps.set("controls", c);
    }
    Json schema = majestic_schema(caps);
    const Json* props = schema.get("properties");
    CCHECK(props);
    if (props) {
        CCHECK(props->get("video0"));
        // mj-settings.js names exactly four sections - image, sensor, video0,
        // video1 - so the substream belongs here; omitting it is the deviation.
        const Json* v1 = props->get("video1");
        CCHECK(v1);
        const Json* f = v1 ? v1->get("properties") : nullptr;
        CCHECK(f && f->get("fps"));
        CCHECK(f && f->get("bitrate_kbps"));
        CCHECK(f && f->get("gop"));
        // ... but as "pipeline", NOT "live". A sub-stream change is not carried
        // by the POST: the unit is rebuilt from the reloaded config, which is
        // what the Apply the page then offers actually delivers. Claiming
        // "live" here would report a change as applied when nothing happened.
        for (const char* k : {"fps", "bitrate_kbps", "gop"}) {
            const Json* fld = f ? f->get(k) : nullptr;
            const Json* xr = fld ? fld->get("x-reload") : nullptr;
            CCHECK(xr && xr->is_string() && xr->as_string() == "pipeline");
        }
        // the main stream stays "live" - its POST really does carry it
        const Json* v0f = props->get("video0") ? props->get("video0")->get("properties") : nullptr;
        const Json* v0b = v0f ? v0f->get("bitrate_kbps") : nullptr;
        CCHECK(v0b && v0b->get("x-reload") && v0b->get("x-reload")->as_string() == "live");
        // and it is offered in a group, or the page never renders it
        const Json* groups = schema.get("x-groups");
        bool offered = false;
        if (groups && groups->is_array())
            for (size_t i = 0; i < groups->size(); ++i) {
                const Json* ss = groups->at(i).get("sections");
                if (!ss || !ss->is_array()) continue;
                for (size_t k = 0; k < ss->size(); ++k)
                    if (ss->at(k).is_string() && ss->at(k).as_string() == "video1") offered = true;
            }
        CCHECK(offered);
    }

    // The translation: video0 and video1 in ONE post must both survive. They
    // land under the same "video" object, so a naive implementation has the
    // second overwrite the first and silently drops half the form.
    {
        Json body = Json::object();
        Json a = Json::object(); a.set("bitrate_kbps", Json::integer(3000));
        Json b = Json::object(); b.set("bitrate_kbps", Json::integer(512));
        body.set("video0", a);
        body.set("video1", b);
        MajesticTranslation t = majestic_post_to_native(body.dump());
        CCHECK(t.ok);
        const Json* v = t.patch.get("video");
        CCHECK(v && v->is_object());
        CCHECK(v && v->get("0") && v->get("0")->get("bitrate_kbps"));
        CCHECK(v && v->get("1") && v->get("1")->get("bitrate_kbps"));
        if (v && v->get("0") && v->get("1")) {
            CCHECK(v->get("0")->get("bitrate_kbps")->as_int() == 3000);
            CCHECK(v->get("1")->get("bitrate_kbps")->as_int() == 512);
        }
    }
    // the other order, because "works one way round" is not the contract
    {
        Json body = Json::object();
        Json b = Json::object(); b.set("gop", Json::integer(40));
        Json a = Json::object(); a.set("gop", Json::integer(20));
        body.set("video1", b);
        body.set("video0", a);
        MajesticTranslation t = majestic_post_to_native(body.dump());
        const Json* v = t.patch.get("video");
        CCHECK(v && v->get("0") && v->get("1"));
    }
    // a non-object section is still refused, and names the right path
    {
        Json body = Json::object();
        body.set("video1", Json::integer(7));
        MajesticTranslation t = majestic_post_to_native(body.dump());
        CCHECK(!t.ok);
        CCHECK(t.path == "video1");
    }
}

// AP14: the snapshot gate must be VISIBLE in the majestic-shaped config.
// dashboard.js:
//   if (mjGet(cfg, 'jpeg.enabled') !== true) {
//       off.textContent = 'Snapshots are disabled - open Live for video';
//       return;
//   }
// With no jpeg section the tile behaved correctly by accident - undefined is
// not true - but the gate was invisible, and a camera that later enabled JPEG
// would still never have been polled.
void run_snapshot_gate_tests() {
    // off: the page must be able to READ that it is off
    {
        Json native = Json::object();
        Json jp = Json::object(); jp.set("enabled", Json::boolean(false)); jp.set("quality", Json::integer(80));
        native.set("jpeg", jp);
        Json cfg = majestic_config(native, Json::object());
        const Json* j = cfg.get("jpeg");
        CCHECK(j && j->is_object());
        CCHECK(j && j->get("enabled") && j->get("enabled")->is_bool() && !j->get("enabled")->as_bool());
    }
    // on: the same key carries true, so the tile polls exactly when it should
    {
        Json native = Json::object();
        Json jp = Json::object(); jp.set("enabled", Json::boolean(true));
        native.set("jpeg", jp);
        Json cfg = majestic_config(native, Json::object());
        const Json* j = cfg.get("jpeg");
        CCHECK(j && j->get("enabled") && j->get("enabled")->as_bool());
    }
    // a native config with no jpeg section at all still yields an explicit
    // false rather than an absent key - the gate is never left to chance
    {
        Json cfg = majestic_config(Json::object(), Json::object());
        const Json* j = cfg.get("jpeg");
        CCHECK(j && j->is_object());
        CCHECK(j && j->get("enabled") && j->get("enabled")->is_bool() && !j->get("enabled")->as_bool());
    }
}
