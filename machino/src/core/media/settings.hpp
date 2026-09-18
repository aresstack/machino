// Platform-neutral image and latency settings. These are configuration
// values only; vendor operations live behind IImageControl/IEncoder.
#pragma once
#include <optional>
#include <string>

namespace machino { namespace media {

enum class LatencyProfile : int { Normal = 0, Low, Custom };

inline const char* latency_profile_name(LatencyProfile p) {
    switch (p) {
        case LatencyProfile::Normal: return "normal";
        case LatencyProfile::Low:    return "low";
        case LatencyProfile::Custom:return "custom";
    }
    return "?";
}

inline bool parse_latency_profile(const std::string& s, LatencyProfile& out) {
    if (s == "normal") { out = LatencyProfile::Normal; return true; }
    if (s == "low")    { out = LatencyProfile::Low; return true; }
    if (s == "custom") { out = LatencyProfile::Custom; return true; }
    return false;
}

// Absence means "leave the ISP/tuning-bin default untouched". In
// particular, anti_flicker is not globally forced to 50 Hz; deployments may
// set 0/50/60 explicitly for their region.
struct ImageSettings {
    std::optional<int> brightness, contrast, saturation, sharpness, hue;
    std::optional<int> hflip, vflip, anti_flicker;
    std::optional<int> ae_compensation, highlight_depress, backlight_comp;
    std::optional<int> white_balance_mode, running_mode;
    std::optional<int> temporal_nr, spatial_nr, dpc, defog;
};

// A profile is only a transparent preset. Every optional member is an
// explicit user override and therefore wins over the selected profile.
struct LatencySettings {
    LatencyProfile profile = LatencyProfile::Normal;
    std::optional<int> gop;
    std::optional<int> framesource_buffers;
    std::optional<int> encoder_buffers;
    std::optional<int> consumer_queue_depth;
};

}} // namespace machino::media
