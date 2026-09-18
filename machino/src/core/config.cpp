#include "core/config.hpp"
#include "core/log.hpp"
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace machino {

static const char* MOD = "CONFIG";

static void trim(std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    size_t b = s.find_last_not_of(" \t\r\n");
    s = (a == std::string::npos) ? std::string() : s.substr(a, b - a + 1);
}

static bool to_int(const std::string& v, int& out) {
    char* end = nullptr;
    long n = strtol(v.c_str(), &end, 0);
    if (end == v.c_str() || *end) return false;
    out = (int)n; return true;
}

static bool to_bool(const std::string& v, bool& out) {
    if (v == "1" || v == "true" || v == "yes" || v == "on")  { out = true;  return true; }
    if (v == "0" || v == "false" || v == "no" || v == "off") { out = false; return true; }
    return false;
}

static bool apply(AppConfig& c, const std::string& k, const std::string& v, int line) {
    int n = 0; bool b = false;
#define INT(key, dst, lo, hi) if (k == key) { if (!to_int(v, n) || n < (lo) || n > (hi)) { \
        LOGW(MOD, "line %d: %s=%s out of range [%d..%d] - ignored", line, key, v.c_str(), (int)(lo), (int)(hi)); return true; } dst = n; return true; }
#define BOOL(key, dst) if (k == key) { if (!to_bool(v, b)) { LOGW(MOD, "line %d: %s expects bool", line, key); return true; } dst = b; return true; }
#define STR(key, dst) if (k == key) { dst = v; return true; }

    STR ("platform",           c.platform)
    STR ("sensor.model",       c.sensor.model)
    INT ("sensor.width",       c.sensor.width,  64, 8192)
    INT ("sensor.height",      c.sensor.height, 64, 8192)
    INT ("sensor.fps",         c.sensor.fps,    1, 120)
    INT ("sensor.i2c_bus",     c.bus.i2c_bus,   0, 4)
    INT ("sensor.i2c_addr",    c.bus.i2c_addr,  0, 0x7f)
    INT ("sensor.mclk",        c.bus.mclk,      0, 2)
    INT ("sensor.reset_gpio",  c.bus.reset_gpio, -1, 255)
    INT ("sensor.pwdn_gpio",   c.bus.pwdn_gpio,  -1, 255)

    INT ("video.width",        c.video.width,   64, 8192)
    INT ("video.height",       c.video.height,  64, 8192)
    INT ("video.fps",          c.video.fps,     1, 120)
    INT ("video.gop",          c.video.gop,     1, 1000)
    INT ("video.bitrate",      c.video.bitrate_kbps, 32, 100000)
    INT ("video.profile",      c.video.profile, 0, 2)
    INT ("video.qp",           c.video.qp,      1, 51)
    INT ("video.buffers",      c.video.buffers, 1, 8)
    if (k == "video.rc_mode") {
        if (v == "cbr") c.video.rc = RcMode::Cbr;
        else if (v == "vbr") c.video.rc = RcMode::Vbr;
        else if (v == "fixqp") c.video.rc = RcMode::FixQp;
        else LOGW(MOD, "line %d: video.rc_mode=%s unknown (cbr|vbr|fixqp)", line, v.c_str());
        return true;
    }

    INT ("rtsp.port",          c.rtsp.port,     1, 65535)
    STR ("rtsp.path",          c.rtsp.path)

    BOOL("pipeline.always_on", c.pipeline.always_on)
    INT ("pipeline.grace_ms",  c.pipeline.grace_ms, 0, 600000)
    INT ("pipeline.poll_timeout_ms", c.pipeline.poll_timeout_ms, 10, 5000)

    INT ("log.level",          c.log.level,     0, 3)
    BOOL("log.syslog",         c.log.syslog)
#undef INT
#undef BOOL
#undef STR
    return false;
}

bool load_config(const char* path, AppConfig& cfg, std::string& err) {
    FILE* f = fopen(path, "r");
    if (!f) { err = std::string("cannot open ") + path + ": " + strerror(errno); return false; }
    char buf[512]; int line = 0, applied = 0;
    while (fgets(buf, sizeof buf, f)) {
        ++line;
        std::string s(buf);
        size_t hash = s.find('#'); if (hash != std::string::npos) s.erase(hash);
        trim(s); if (s.empty()) continue;
        size_t eq = s.find('=');
        if (eq == std::string::npos) { LOGW(MOD, "line %d: no '=' - ignored", line); continue; }
        std::string k = s.substr(0, eq), v = s.substr(eq + 1);
        trim(k); trim(v);
        if (v.size() >= 2 && v.front() == '"' && v.back() == '"') v = v.substr(1, v.size() - 2);
        if (!apply(cfg, k, v, line)) LOGW(MOD, "line %d: unknown key '%s' - ignored", line, k.c_str());
        else ++applied;
    }
    fclose(f);
    if (!cfg.rtsp.path.empty() && cfg.rtsp.path[0] != '/') cfg.rtsp.path.insert(0, "/");
    LOGI(MOD, "loaded %d settings from %s (platform %s)", applied, path, cfg.platform.c_str());
    LOGI(MOD, "sensor %s %dx%d@%d i2c%d/0x%02x mclk%d rst=%d pwdn=%d | video %dx%d@%d gop=%d %dkbps",
         cfg.sensor.model.c_str(), cfg.sensor.width, cfg.sensor.height, cfg.sensor.fps,
         cfg.bus.i2c_bus, cfg.bus.i2c_addr, cfg.bus.mclk, cfg.bus.reset_gpio, cfg.bus.pwdn_gpio,
         cfg.video.width, cfg.video.height, cfg.video.fps, cfg.video.gop, cfg.video.bitrate_kbps);
    return true;
}

} // namespace machino
