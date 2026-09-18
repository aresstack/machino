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
};

// Fully determined stream parameters handed to the pipeline.
struct EffectiveStream {
    int    width = 0, height = 0, fps = 0;
    int    native_width = 0, native_height = 0;   // sensor native size (scaler decision)
    int    gop = 40, bitrate_kbps = 3000, profile = 2, qp = 35, buffers = 2;
    RcMode rc = RcMode::Cbr;
};

struct RtspConfig {
    int         port = 554;
    std::string path = "/ch0";
};

// Demand-driven lifecycle ("no consumer, no pipeline").
struct PipelineConfig {
    bool always_on       = false;  // hold a permanent manual demand
    int  idle_grace_ms   = 5000;   // GRACE_IDLE duration after the last consumer left
    int  poll_timeout_ms = 500;    // encoder poll granularity
};

struct LogConfig {
    int  level  = 2;
    bool syslog = false;
};

struct AppConfig {
    hw::UserHardwareConfig hardware;
    std::string     board_profile_file;
    StreamConfig    video;
    RtspConfig      rtsp;
    PipelineConfig  pipeline;
    LogConfig       log;
};

bool load_config(const char* path, AppConfig& cfg, std::string& err);
bool parse_config_text(const std::string& text, AppConfig& cfg, std::string& err);
EffectiveStream effective_stream(const StreamConfig& v, const hw::ResolvedHardware& hw);

} // namespace machino
