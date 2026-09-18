// Machino core: capability model (internal; the M6 API exposes it).
// Tri-state on purpose: unknown != unsupported. An adapter reports only what
// it has actually established; everything else stays Unknown.
//
// M5 adds ranged controls with an apply mode: how a change takes effect.
#pragma once

namespace machino {

enum class Cap : int { Unknown = 0, Supported = 1, Unsupported = 2 };

inline const char* cap_name(Cap c) {
    switch (c) {
        case Cap::Supported:   return "supported";
        case Cap::Unsupported: return "unsupported";
        case Cap::Unknown:     return "unknown";
    }
    return "?";
}

// How a setting change takes effect. Nothing is ever silently ignored: a
// control that cannot be applied reports Unsupported.
enum class ApplyMode : int { Live = 0, PipelineRestart, DaemonRestart, BootOnly, Unsupported };

inline const char* apply_mode_name(ApplyMode m) {
    switch (m) {
        case ApplyMode::Live:            return "live";
        case ApplyMode::PipelineRestart: return "pipeline-restart";
        case ApplyMode::DaemonRestart:   return "daemon-restart";
        case ApplyMode::BootOnly:        return "boot-only";
        case ApplyMode::Unsupported:     return "unsupported";
    }
    return "?";
}

// A numeric control with optional known bounds (-1 = unknown, never invented).
struct RangeCap {
    Cap       support = Cap::Unknown;
    int       min = -1;
    int       max = -1;
    ApplyMode apply = ApplyMode::Unsupported;
    bool in_range(int v) const { return (min < 0 || v >= min) && (max < 0 || v <= max); }
};

// A performance/clock control without a numeric range (levels: auto/low/high).
struct PerfCap {
    Cap       support = Cap::Unknown;
    ApplyMode apply = ApplyMode::Unsupported;
    bool      readable = false;      // current value can be read (telemetry)
};

struct CapabilitySet {
    struct Video   { Cap h264 = Cap::Unknown; Cap h265 = Cap::Unknown; int max_streams = -1;
                     RangeCap fps;        // stream / FrameSource output fps
                     RangeCap bitrate;    // kbps
                   } video;
    struct Sensor  { Cap configurable_fps = Cap::Unknown; RangeCap fps; } sensor;
    struct Isp     { Cap available = Cap::Unknown; PerfCap performance; } isp;
    struct Encoder { Cap hardware = Cap::Unknown; PerfCap performance; } encoder;
    struct Power   { Cap isp_clock_control = Cap::Unknown; Cap encoder_clock_control = Cap::Unknown;
                     Cap cpu_frequency_control = Cap::Unknown; PerfCap cpu_frequency; } power;
    struct Ai      { Cap available = Cap::Unknown; } ai;
};

} // namespace machino
