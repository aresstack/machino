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
#include "ports/iosd.hpp"
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
    bool   osd          = true;   // majestic video0.osd: draw the overlay here
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
    bool   osd          = true;   // majestic video1.osd
};

// M8: JPEG snapshots. The encoder is ephemeral - created on demand, torn down
// after snapshot.grace_ms - never kept alive because the endpoint exists.
struct JpegConfig {
    // OFF by default: creating the extra FrameSource/encoder pair live-wedged
    // the whole daemon on the T40NN (IMP hang under the manager lock, RTSP and
    // API dead until power-cycle). Stays opt-in until it is hardware-verified.
    bool enabled = false;
    int quality = 80;              // 1..99
};

struct SnapshotConfig {
    int cache_ms = 300;            // serve the same JPEG to near-simultaneous requests
    int grace_ms = 2000;           // keep the hardware encoder warm this long after the last capture
};

// RTSP authentication. The stock webui states these endpoints authenticate as
// root with the WebUI password, so there is no separate RTSP account here -
// only the switch and which wire scheme is offered. See app/rtsp/rtsp_auth.hpp
// for why Digest needs a stored secret while Basic works with /etc/shadow.
struct RtspAuthConfig {
    // AP7: ON by default, because that is what the stock WebUI tells the user
    // is happening. stream-urls.cgi carries two mutually exclusive notes and
    // main.js picks between them on ONE key:
    //
    //   const unsafe = mjGet(cfg, 'system.unsafe');
    //   const note = $(unsafe === true || unsafe === 'true' ? '#ep-unsafe' : '#ep-auth');
    //
    // so upstream has no rtsp.auth key at all: RTSP authenticates unless
    // system.unsafe is set, full stop. Shipping this false meant the page said
    // "These endpoints authenticate as user root" while the camera streamed to
    // anyone who asked - a silent, invisible drop-in deviation.
    //
    // `system.unsafe` still outranks this, as it outranks everything.
    bool        enabled = true;
    bool        offer_basic = true;       // the only scheme /etc/shadow can serve
    bool        offer_digest = false;     // needs an HA1 provider (a stored secret)
    std::string realm = "Machino";
    int         nonce_lifetime_s = 300;
};

struct RtspConfig {
    bool        enabled = true;            // false: no listener at all (majestic rtsp.enabled)
    int         port = 554;
    std::string path = "/ch0";
    std::string sub_path = "/ch1";         // substream mount point (when video.1 is enabled)
    int         send_buffer_bytes = 65536; // bounded kernel backlog per socket
    int         send_stall_ms = 750;       // disconnect, never accumulate seconds of stale live video
    int         max_clients = 4;           // concurrent connections (each costs a thread); refused, not queued
    RtspAuthConfig auth;
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

// M9: detection / AI. Off by default. A backend that needs an external model
// takes model_path; motion (IMP_IVS) does not.
struct AiConfig {
    bool        enabled = false;
    std::string detector = "motion";   // backend selector; "motion" = IMP_IVS move
    int         inference_fps = 5;      // analysis cadence, independent of video fps
    std::string model_path;            // only for model-based backends
};

struct TelemetryConfig {
    int log_interval_s = 0;        // 0 = off; otherwise one compact line every N seconds
};

// AP9: on-screen display. Field names, types and defaults are taken from the
// section the unmodified majestic-webui renders (`osd` in its schema), not
// invented here - `size`, `offsetX` and `offsetY` really are strings upstream,
// because they carry a unit ("1.5em", "2%"), and keeping them strings is what
// makes a GET/SET round trip through the stock page lossless.
//
// The CONTENT is global and the per-stream `video<N>.osd` booleans only say
// where it is drawn. That is upstream's model exactly; per-stream text is not
// offered here because the page has no way to set it.
struct OsdConfig {
    bool        enabled  = false;
    std::string tmpl     = "%d.%m.%Y %H:%M:%S";    // osd.template
    std::string font     = "/usr/share/fonts/truetype/UbuntuMono-Regular.ttf";
    std::string size     = "1.0";                  // font scale factor
    bool        thin     = false;                  // osd.weight: normal | thin
    bool        outline  = true;
    OsdAnchor   anchor   = OsdAnchor::Proportional;
    std::string offset_x = "0";
    std::string offset_y = "0";
    int         pos_x    = 16;                     // proportional grid, 16 left .. -16 right
    int         pos_y    = 16;                     // 16 top .. -16 bottom
    int         bg_alpha = 25;                     // plate opacity, percent
    // Where uploaded logo overlays live. One defined persistent path, never a
    // caller-supplied one: the filename is derived from the overlay index.
    std::string image_dir = "/etc/machino/osd";
};

// HTTP control/telemetry API (M6). Reads are never media demand.
struct ApiConfig {
    bool        enabled = true;
    std::string bind = "0.0.0.0";
    int         port = 8080;
    // Front-door mode: when Machino owns the public port (80) it serves the
    // Majestic-facing routes natively and relays every OTHER request to the
    // internal OpenIPC WebUI (busybox httpd) here. 0 = no relay (stand-alone
    // API only), which is the dev/test default.
    std::string upstream_host = "127.0.0.1";
    int         upstream_port = 0;
    // Majestic drop-in session login (POST /login against the system account)
    // on the front door. Only effective when upstream_port > 0.
    bool        auth = true;
};

// majestic `system` section. `unsafe` is upstream's "Disable authentication"
// switch (schema: boolean, default false). Upstream is explicit about its
// reach: it "overrides everything, unclaimed cameras included - that, not a
// blank password, is how a deliberately-open camera is configured". So it
// turns off the session gate AND the unclaimed redirect, and no other meaning
// is invented for it here.
// AP11 ONVIF. Field names and meanings from the section the stock settings
// page renders. `password` is CLEARTEXT and opt-in, exactly as upstream says:
// it is what unlocks WSSE PasswordDigest, because /etc/shadow cannot produce
// the cleartext a digest has to be recomputed from.
//
// Default DIFFERS from upstream, which defaults enabled=true: this stack has
// never run against a real ONVIF client, and a half-answering camera on the
// network is worse than a silent one. It goes to true when it is
// hardware-accepted.
struct OnvifConfig {
    bool        enabled  = false;
    std::string username = "root";
    std::string password;          // cleartext; empty = /etc/shadow only
};

struct SystemConfig {
    bool unsafe = false;
};

// AP4. The names and the default are majestic's, read off this camera's own
// /etc/majestic.yaml rather than invented:
//
//   watchdog:
//     enabled: true
//     timeout: 15
//
// Deliberately NOT added to the WebUI schema: upstream does not offer it
// either, and a switch that turns off a camera's only automatic recovery does
// not belong one click away.
struct WatchdogConfig {
    bool enabled = true;
    int  timeout_s = 15;
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
    AiConfig          ai;
    OsdConfig         osd;
    SystemConfig      system;
    WatchdogConfig    watchdog;
    OnvifConfig       onvif;
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
