// Compatibility surface for OpenIPC/majestic-webui.
//
// This is deliberately an adapter at the HTTP boundary. Machino's native
// /api/v1 contract stays platform-neutral and keeps its own shape; the WebUI
// adapter only presents the legacy schema/config documents and translates
// writes back into native partial patches.
#pragma once
#include "core/json.hpp"
#include <cstdint>
#include <string>
#include <vector>

namespace machino { namespace compat {

// A snapshot of the Linux side that majestic-webui's Dashboard reads from
// /metrics (node-exporter names). Filled by the app layer from /proc + sysfs
// so this compat unit stays pure and host-testable. Fields left at their
// "have_*" = false are simply omitted from the output.
struct LinuxSample {
    double now_unix = 0;                 // node_time_seconds
    bool   have_boot = false; double boot_unix = 0;       // node_boot_time_seconds
    bool   have_app_boot = false; double app_boot_unix = 0; // app_boot_time_seconds (daemon start)
    bool   have_load = false; double load1 = 0, load5 = 0, load15 = 0;
    bool   have_mem = false;
    uint64_t mem_total = 0, mem_free = 0, mem_avail = 0,
             mem_sreclaim = 0, mem_active_file = 0, mem_inactive_file = 0;   // bytes
    bool   have_temp = false; double temp_c = 0;          // node_hwmon_temp_celsius
    struct Cpu { int index = 0; uint64_t user=0,nice=0,system=0,idle=0,iowait=0,irq=0,softirq=0,steal=0; };
    std::vector<Cpu> cpus;               // node_cpu_seconds_total{cpu,mode}
    struct Net { std::string dev; uint64_t rx = 0, tx = 0; };
    std::vector<Net> nets;               // node_network_{receive,transmit}_bytes_total{device}
};

struct MajesticTranslation {
    bool ok = false;
    int status = 400;
    std::string code;
    std::string path;
    std::string message;
    Json patch = Json::object();
    // reset of a no-default field: these machino.conf keys are REMOVED
    // (unset state) instead of patched - mj-settings.js #416 contract.
    std::vector<std::string> unset;
};

// Build the schema shape consumed by majestic-webui's mj-settings.js.
// Only controls Machino reports as supported are advertised.
Json majestic_schema(const Json& capabilities);

// Flatten Machino's native config into the one-section-deep shape the current
// majestic-webui renderer expects (notably video.0 -> video0). Effective state
// fills optional image controls that have no explicit requested value yet.
Json majestic_config(const Json& native_config, const Json& state);

// Translate a majestic-webui POST body back into a native Machino PATCH body.
// Validation of values remains exclusively in ApiService::patch_config().
MajesticTranslation majestic_post_to_native(const std::string& body);

// Prometheus text (node-exporter names) for majestic-webui's Dashboard
// heartbeat. Combines the Linux sample with Machino telemetry/state so CPU,
// memory, uptime, network, ISP and encoder tiles populate and the
// "Camera is not responding" banner (a failing /metrics poll) clears.
std::string majestic_metrics(const Json& telemetry, const Json& state, const LinuxSample& lin);

// The {"sources":[...]} document majestic-webui fetches. WIRE FORMAT taken
// from the upstream tests (tests/sources.test.js, fixture FROM_A_REAL_CAMERA):
// sources[] = {camera, kind:"sensor"|"external", streams:[{id = 3*camera+sub,
// subtype:"main"|"sub"|"mjpeg" (a NAME, not an index), codec, fps, width,
// height, flowing (h264 only), configured, present, rtsp}]}.
Json majestic_sources(const Json& majestic_config, const Json& state);

// GET /api/v1/get?key=<dotted> - the camera-local config probe the stock CGIs
// use (www/cgi-bin/p/majestic.sh mj_cfg): plain-text value on 200, miss = 404.
bool majestic_get(const Json& majestic_config, const std::string& key, std::string& out_text);

// GET /api/v1/reset?key=<dotted> - the settings page's per-row reset. Returns
// a native PATCH restoring the built-in default; !ok with status 404 means
// "this camera has no such (resettable) setting" (the UI handles that).
MajesticTranslation majestic_reset(const std::string& key);

}} // namespace machino::compat
