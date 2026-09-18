// Machino core: capability model (internal only in M3; the M6 API exposes it).
// Tri-state on purpose: unknown != unsupported. An adapter reports only what
// it has actually established; everything else stays Unknown.
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

struct CapabilitySet {
    struct Video   { Cap h264 = Cap::Unknown; Cap h265 = Cap::Unknown; int max_streams = -1; /* -1 = unknown */ } video;
    struct Sensor  { Cap configurable_fps = Cap::Unknown; } sensor;
    struct Isp     { Cap available = Cap::Unknown; } isp;
    struct Encoder { Cap hardware = Cap::Unknown; } encoder;
    struct Power   { Cap isp_clock_control = Cap::Unknown; Cap encoder_clock_control = Cap::Unknown;
                     Cap cpu_frequency_control = Cap::Unknown; } power;
    struct Ai      { Cap available = Cap::Unknown; } ai;
};

} // namespace machino
