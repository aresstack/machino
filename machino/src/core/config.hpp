// Machino core: application configuration. Flat `section.key = value` file.
//
// Hardware description is layered (see core/hw/resolve.hpp):
//   board = <profile id>          selects a registered board profile
//   board_profile_file = <path>   loads a profile from a file (registered under its board_id)
//   platform / sensor.* keys      explicit user values; they override the profile
// Every hardware key is optional here: "not set" is a real state, so the
// resolver can apply user > board > platform-default > fail-closed.
#pragma once
#include "core/hw/resolve.hpp"
#include "core/media/settings.hpp"
#include "core/power/apply.hpp"
#include <optional>
#include <string>

namespace machino {

enum class RcMode : int { Cbr = 0, Vbr = 1, FixQp = 2 };

// Encoder/stream settings. width/height/fps unset -> taken from the resolved
// sensor mode.
struct StreamConfig {
    std::optional<int> width, height, fps;
    int    gop          = 40;
    int    bitrate_kbps = 3000;
    int    profile      = 2;      // 0 baseline, 1 main, 2 high
    RcMode rc           = RcMode::Cbr;
    int    qp           = 35;     // FixQp only
    int    buffers      = 2;      // FrameSource video buffers
    int    encoder_buffers = 0;   // 0 = vendor default; otherwise stream buffers
};

// Fully determined stream parameters handed to the pipeline.
struct EffectiveStream {
    int    width = 0, height = 0, fps = 0;
    int    native_width = 0, native_height = 0;   // sensor native size (scaler decision)
    int    gop = 40, bitrate_kbps = 3000, profile = 2, qp = 35, buffers = 2, encoder_buffers = 0;
    RcMode rc = RcMode::Cbr;
};

// M8: the substream is off by default and has NO invented geometry: enabling
// it requires an explicit width/height (or a board preset that provides one) -
// the resolver refuses to guess, like everywhere else.
struct SubStreamConfig {
    bool enabled = false;
    std::optional<int> width, height, fps;   // fps unset = follow the main stream
    int    gop          = 40;
    int    bitrate_kbps = 512;
    int    profile      = 2;
    RcMode rc           = RcMode::Cbr;
    int    qp           = 35;
    int    buffers      = 2;
    int    encoder_buffers = 0;
};

// M8: JPEG snapshots. The encoder is ephemeral - created on demand, torn down
// after snapshot.grace_ms - never kept alive because the endpoint exists.
struct JpegConfig {
    int quality = 80;              // 1..99
};

struct SnapshotConfig {
    int cache_ms = 300;            // serve the same JPEG to near-simultaneous requests
    int grace_ms = 2000;           // keep the hardware encoder warm this long after the last capture
};

struct RtspConfig {
    int         port = 554;
    std::string path = "/ch0";
    std::string sub_path = "/ch1";         // substream mount point (when video.1 is enabled)
    int         send_buffer_bytes = 65536; // bounded kernel backlog per socket
    int         send_stall_ms = 750;       // disconnect, never accumulate seconds of stale live video
    int         max_clients = 4;           // concurrent connections (each costs a thread); refused, not queued
};

// Demand-driven lifecycle ("no consumer, no pipeline").
struct PipelineConfig {
    bool always_on       = false;  // hold a permanent manual demand
    int  idle_grace_ms   = 5000;   // GRACE_IDLE duration after the last consumer left
    int  poll_timeout_ms = 500;    // encoder poll granularity
};

// Power / performance (M5). Levels: auto = adapter/kernel default, never forced.
struct PerformanceConfig {
    power::Profile   profile   = power::Profile::Performance;
    int              sensor_fps = 0;                 // 0 = follow the mode / stream fps
    power::PerfLevel isp = power::PerfLevel::Auto, encoder = power::PerfLevel::Auto, cpu = power::PerfLevel::Auto;
};

struct TelemetryConfig {
    int log_interval_s = 0;        // 0 = off; otherwise one compact line every N seconds
};

// HTTP control/telemetry API (M6). Reads are never media demand.
struct ApiConfig {
    bool        enabled = true;
    std::string bind = "0.0.0.0";
    int         port = 8080;
};

struct LogConfig {
    int  level  = 2;
    bool syslog = false;
};

struct AppConfig {
    hw::UserHardwareConfig hardware;
    std::string       board_profile_file;
    StreamConfig      video;
    SubStreamConfig   video1;
    JpegConfig        jpeg;
    SnapshotConfig    snapshot;
    RtspConfig        rtsp;
    PipelineConfig    pipeline;
    PerformanceConfig performance;
    media::ImageSettings image;
    media::LatencySettings latency;
    TelemetryConfig   telemetry;
    ApiConfig         api;
    LogConfig         log;
    unsigned          revision = 1;     // config.revision (managed by the ConfigStore)
};

bool load_config(const char* path, AppConfig& cfg, std::string& err);
bool parse_config_text(const std::string& text, AppConfig& cfg, std::string& err);
EffectiveStream effective_stream(const StreamConfig& v, const hw::ResolvedHardware& hw);
// Substream geometry is validated, not invented: returns false with `err` when
// video.1 is enabled without an explicit width/height, or when it does not fit
// into the sensor's native size.
bool effective_sub_stream(const SubStreamConfig& v, const EffectiveStream& main,
                          EffectiveStream& out, std::string& err);

} // namespace machino
