// M10: one-way majestic.yaml -> machino migration. Proves the classification
// (mapped/converted/ignored/unsupported/invalid), the value transforms
// (GOP seconds->frames, image 0..100->0..255, resolution split, motionDetect
// -> AI), and honest failure on garbage.
#include "app/compat/majestic_migrate.hpp"

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

} // namespace

void run_compat_tests() {
    test_migrate_core();
    test_migrate_edges();
}
