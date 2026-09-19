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

    Json src = majestic_sources(cfg);
    const Json* arr = src.get("sources");
    CCHECK(arr && arr->is_array() && arr->size() == 1);
    if (arr && arr->size() == 1) {
        const Json& e = arr->at(0);
        CCHECK(e.get("id") && e.get("id")->as_string() == "video0");
        CCHECK(e.get("camera") && e.get("camera")->as_int() == 1);
        CCHECK(e.get("size") && e.get("size")->as_string() == "1920x1080");
    }
}

} // namespace

void run_compat_tests() {
    test_migrate_core();
    test_migrate_edges();
    test_webui_metrics();
    test_webui_config_and_sources();
}
