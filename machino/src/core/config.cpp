#include "core/config.hpp"
#include "core/hw/board_profile_parser.hpp"
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

static bool to_gpio(const std::string& v, int& out) {
    if (v == "none" || v == "-1") { out = -1; return true; }
    return to_int(v, out) && out >= 0 && out <= 255;
}

static bool apply(AppConfig& c, const std::string& k, const std::string& v, int line) {
    int n = 0; bool b = false;
#define INT(key, dst, lo, hi) if (k == key) { if (!to_int(v, n) || n < (lo) || n > (hi)) { \
        LOGW(MOD, "line %d: %s=%s out of range [%d..%d] - ignored", line, key, v.c_str(), (int)(lo), (int)(hi)); return true; } dst = n; return true; }
#define OPTINT(key, dst, lo, hi) INT(key, dst, lo, hi)
#define GPIO(key, dst) if (k == key) { if (!to_gpio(v, n)) { LOGW(MOD, "line %d: %s=%s invalid (pin number or 'none') - ignored", line, key, v.c_str()); return true; } dst = n; return true; }
#define BOOL(key, dst) if (k == key) { if (!to_bool(v, b)) { LOGW(MOD, "line %d: %s expects bool", line, key); return true; } dst = b; return true; }
#define STR(key, dst) if (k == key) { dst = v; return true; }

    STR   ("board",              c.hardware.board_id)
    STR   ("board_profile_file", c.board_profile_file)
    STR   ("platform",           c.hardware.platform)
    STR   ("sensor.model",       c.hardware.sensor)
    OPTINT("sensor.i2c_bus",     c.hardware.wiring.i2c_bus,  0, 15)
    OPTINT("sensor.i2c_addr",    c.hardware.wiring.i2c_addr, 0, 0x7f)
    OPTINT("sensor.mclk",        c.hardware.wiring.mclk,     0, 7)
    GPIO  ("sensor.reset_gpio",  c.hardware.wiring.reset_gpio)
    GPIO  ("sensor.pwdn_gpio",   c.hardware.wiring.pwdn_gpio)
    BOOL  ("sensor.allow_unverified_mode", c.hardware.allow_unverified_mode)
    if (k == "sensor.mode") {
        hw::SensorMode m;
        if (!hw::parse_mode(v, m)) { LOGW(MOD, "line %d: sensor.mode=%s invalid (WxH@fps)", line, v.c_str()); return true; }
        c.hardware.mode = m; return true;
    }
    // sensor.fps = requested SENSOR frame rate (M5 control); sensor.width/height
    // remain legacy parts of an explicit mode
    INT   ("sensor.fps",         c.performance.sensor_fps, 1, 120)
    if (k == "sensor.width" || k == "sensor.height") {
        if (!to_int(v, n) || n <= 0) { LOGW(MOD, "line %d: %s invalid", line, k.c_str()); return true; }
        hw::SensorMode m = c.hardware.mode.value_or(hw::SensorMode{});
        if (k == "sensor.width") m.width = n; else m.height = n;
        c.hardware.mode = m; return true;
    }

    OPTINT("video.width",        c.video.width,   64, 8192)
    OPTINT("video.height",       c.video.height,  64, 8192)
    OPTINT("video.fps",          c.video.fps,     1, 120)
    OPTINT("video0.fps",         c.video.fps,     1, 120)
    INT   ("video.gop",          c.video.gop,     1, 1000)
    INT   ("video.bitrate",      c.video.bitrate_kbps, 32, 100000)
    INT   ("video0.bitrate",     c.video.bitrate_kbps, 32, 100000)
    INT   ("video.profile",      c.video.profile, 0, 2)
    INT   ("video.qp",           c.video.qp,      1, 51)
    INT   ("video.buffers",      c.video.buffers, 1, 8)
    INT   ("video.encoder_buffers", c.video.encoder_buffers, 1, 8)
    if (k == "video.rc_mode") {
        if (v == "cbr") c.video.rc = RcMode::Cbr;
        else if (v == "vbr") c.video.rc = RcMode::Vbr;
        else if (v == "fixqp") c.video.rc = RcMode::FixQp;
        else LOGW(MOD, "line %d: video.rc_mode=%s unknown (cbr|vbr|fixqp)", line, v.c_str());
        return true;
    }

    if (k == "latency.profile") {
        if (!media::parse_latency_profile(v, c.latency.profile))
            LOGW(MOD, "line %d: latency.profile=%s unknown (normal|low|custom)", line, v.c_str());
        return true;
    }
    OPTINT("latency.gop",                 c.latency.gop,                 1, 1000)
    OPTINT("latency.framesource_buffers", c.latency.framesource_buffers, 1, 8)
    OPTINT("latency.encoder_buffers",     c.latency.encoder_buffers,     1, 8)
    OPTINT("latency.queue_depth",         c.latency.consumer_queue_depth, 1, 32)

    OPTINT("image.brightness",        c.image.brightness,        0, 255)
    OPTINT("image.contrast",          c.image.contrast,          0, 255)
    OPTINT("image.saturation",        c.image.saturation,        0, 255)
    OPTINT("image.sharpness",         c.image.sharpness,         0, 255)
    OPTINT("image.hue",               c.image.hue,               0, 255)
    OPTINT("image.hflip",             c.image.hflip,             0, 1)
    OPTINT("image.vflip",             c.image.vflip,             0, 1)
    if (k == "image.anti_flicker") {
        if (v == "off") c.image.anti_flicker = 0;
        else if (v == "50hz") c.image.anti_flicker = 50;
        else if (v == "60hz") c.image.anti_flicker = 60;
        else LOGW(MOD, "line %d: image.anti_flicker=%s unknown (off|50hz|60hz)", line, v.c_str());
        return true;
    }
    OPTINT("image.ae_compensation",    c.image.ae_compensation,    0, 255)
    OPTINT("image.highlight_depress",  c.image.highlight_depress,  0, 10)
    OPTINT("image.backlight_comp",     c.image.backlight_comp,     0, 10)
    OPTINT("image.white_balance_mode",c.image.white_balance_mode, 0, 9)
    OPTINT("image.running_mode",       c.image.running_mode,       0, 1)
    OPTINT("image.temporal_nr",        c.image.temporal_nr,        0, 1)
    OPTINT("image.spatial_nr",         c.image.spatial_nr,         0, 1)
    OPTINT("image.dpc",                c.image.dpc,                0, 1)
    OPTINT("image.defog",              c.image.defog,              0, 1)

    INT   ("rtsp.port",          c.rtsp.port,     1, 65535)
    STR   ("rtsp.path",          c.rtsp.path)
    INT   ("rtsp.send_buffer_bytes", c.rtsp.send_buffer_bytes, 4096, 1048576)
    INT   ("rtsp.send_stall_ms", c.rtsp.send_stall_ms, 50, 10000)

    BOOL  ("pipeline.always_on", c.pipeline.always_on)
    INT   ("lifecycle.idle_grace_ms", c.pipeline.idle_grace_ms, 0, 600000)
    INT   ("pipeline.grace_ms",  c.pipeline.idle_grace_ms, 0, 600000)
    INT   ("pipeline.poll_timeout_ms", c.pipeline.poll_timeout_ms, 10, 5000)

    if (k == "performance.profile") {
        if (!power::parse_profile(v, c.performance.profile)) LOGW(MOD, "line %d: performance.profile=%s unknown (performance|balanced|battery|custom)", line, v.c_str());
        return true;
    }
    if (k == "power.isp_performance" || k == "power.encoder_performance" || k == "power.cpu_performance") {
        power::PerfLevel l;
        if (!power::parse_perf_level(v, l)) { LOGW(MOD, "line %d: %s=%s unknown (auto|low|high) - ignored", line, k.c_str(), v.c_str()); return true; }
        if (k == "power.isp_performance") c.performance.isp = l;
        else if (k == "power.encoder_performance") c.performance.encoder = l;
        else c.performance.cpu = l;
        return true;
    }
    INT   ("telemetry.log_interval_s", c.telemetry.log_interval_s, 0, 3600)

    BOOL  ("api.enabled",        c.api.enabled)
    STR   ("api.bind",           c.api.bind)
    INT   ("api.port",           c.api.port,      1, 65535)
    if (k == "config.revision") { if (to_int(v, n) && n > 0) c.revision = (unsigned)n; return true; }

    INT   ("log.level",          c.log.level,     0, 3)
    BOOL  ("log.syslog",         c.log.syslog)
#undef INT
#undef OPTINT
#undef GPIO
#undef BOOL
#undef STR
    return false;
}

bool parse_config_text(const std::string& text, AppConfig& cfg, std::string& err) {
    (void)err;
    size_t pos = 0; int line = 0, applied = 0;
    while (pos < text.size()) {
        size_t nl = text.find('\n', pos);
        std::string s = text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        pos = (nl == std::string::npos) ? text.size() : nl + 1;
        ++line;
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
    if (!cfg.rtsp.path.empty() && cfg.rtsp.path[0] != '/') cfg.rtsp.path.insert(0, "/");
    if (cfg.hardware.mode && (cfg.hardware.mode->width <= 0 || cfg.hardware.mode->height <= 0)) {
        LOGW(MOD, "sensor.width/height incomplete - ignoring partial sensor mode (use sensor.mode = WxH@fps)");
        cfg.hardware.mode.reset();
    }
    if (cfg.hardware.mode && cfg.hardware.mode->fps <= 0) {
        // legacy: mode given as width/height only -> fps from sensor.fps or the profile default
        if (cfg.performance.sensor_fps > 0) cfg.hardware.mode->fps = cfg.performance.sensor_fps;
        else { LOGW(MOD, "sensor.width/height without fps - ignoring partial sensor mode"); cfg.hardware.mode.reset(); }
    }
    LOGI(MOD, "loaded %d settings", applied);
    return true;
}

bool load_config(const char* path, AppConfig& cfg, std::string& err) {
    FILE* f = fopen(path, "r");
    if (!f) { err = std::string("cannot open ") + path + ": " + strerror(errno); return false; }
    std::string text; char buf[512];
    while (fgets(buf, sizeof buf, f)) text += buf;
    fclose(f);
    if (!parse_config_text(text, cfg, err)) return false;
    LOGI(MOD, "config %s: board='%s' platform='%s' sensor='%s' profile=%s", path, cfg.hardware.board_id.c_str(),
         cfg.hardware.platform.c_str(), cfg.hardware.sensor.c_str(), power::profile_name(cfg.performance.profile));
    return true;
}

EffectiveStream effective_stream(const StreamConfig& v, const hw::ResolvedHardware& hw) {
    EffectiveStream e;
    e.width  = v.width.value_or(hw.mode.value.width);
    e.height = v.height.value_or(hw.mode.value.height);
    e.fps    = v.fps.value_or(hw.mode.value.fps);
    e.native_width  = hw.sensor.native_width  > 0 ? hw.sensor.native_width  : hw.mode.value.width;
    e.native_height = hw.sensor.native_height > 0 ? hw.sensor.native_height : hw.mode.value.height;
    e.gop = v.gop; e.bitrate_kbps = v.bitrate_kbps; e.profile = v.profile; e.qp = v.qp; e.buffers = v.buffers;
    e.encoder_buffers = v.encoder_buffers; e.rc = v.rc;
    return e;
}

} // namespace machino
