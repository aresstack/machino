// Port: image (ISP) control. Capability-based: the adapter reports which
// controls exist, their ranges and how they apply; the core never sees an
// IMP_ISP_Tuning_* call. All values are plain integers so the API can carry
// them unchanged; enumerated controls document their integer encoding.
#pragma once
#include "core/capabilities.hpp"
#include "core/result.hpp"
#include <cstdint>

namespace machino {

enum class ImageControl : int {
    Brightness = 0,     // 0..255, 128 = neutral
    Contrast,           // 0..255
    Saturation,         // 0..255
    Sharpness,          // 0..255
    Hue,                // 0..255
    HFlip,              // 0/1
    VFlip,              // 0/1
    AntiFlicker,        // 0 = off, 50 = 50 Hz, 60 = 60 Hz
    AeCompensation,     // AE luma target compensation 0..255 (128 = neutral), 0 = disabled
    HighlightDepress,   // 0..10 (0 = off)
    BacklightComp,      // 0..10 (0 = off)
    WhiteBalanceMode,  // 0 auto, 1 manual, 2..9 documented ISP presets
    RunningMode,        // 0 = day, 1 = night
    TemporalNr,         // 0/1 enable (temporal/motion denoise)
    SpatialNr,          // 0/1 enable
    Dpc,                // 0/1 enable (defective pixel correction)
    Defog,              // 0/1 enable
    Wdr,                // capability slot; current linear IMX307 mode is not verified for WDR
    Drc,                // capability slot; SDK 1.3.1 marks module interface reserved
    COUNT
};

inline const char* image_control_name(ImageControl c) {
    switch (c) {
        case ImageControl::Brightness: return "brightness";       case ImageControl::Contrast: return "contrast";
        case ImageControl::Saturation: return "saturation";       case ImageControl::Sharpness: return "sharpness";
        case ImageControl::Hue: return "hue";                     case ImageControl::HFlip: return "hflip";
        case ImageControl::VFlip: return "vflip";                 case ImageControl::AntiFlicker: return "anti_flicker";
        case ImageControl::AeCompensation: return "ae_compensation"; case ImageControl::HighlightDepress: return "highlight_depress";
        case ImageControl::BacklightComp: return "backlight_comp"; case ImageControl::RunningMode: return "running_mode";
        case ImageControl::WhiteBalanceMode: return "white_balance_mode";
        case ImageControl::TemporalNr: return "temporal_nr";      case ImageControl::SpatialNr: return "spatial_nr";
        case ImageControl::Dpc: return "dpc";                     case ImageControl::Defog: return "defog";
        case ImageControl::Wdr: return "wdr";                     case ImageControl::Drc: return "drc";
        case ImageControl::COUNT: break;
    }
    return "?";
}

struct ImageCaps {
    RangeCap control[(int)ImageControl::COUNT];   // support/min/max/apply per control
};

// Auto-exposure read-back (what the ISP is actually doing).
// Two groups, because they come from two independent reads and one of them can
// fail on its own. On the T40NN that is not hypothetical: the scene read
// answers (luma 53, target 50, converged) while the exposure read gives
// nothing. With a single `available` flag the caller could not tell the
// difference, so it published the untouched zeros -- and `isp_exptime 0` is a
// statement about the sensor, not an absence of one. Majestic reports 78964
// there. A number nobody measured is worse than no number: the reader cannot
// see that it is missing.
struct ExposureReadback {
    bool     available = false;   // either group answered
    bool     have_scene = false;  // luma / target / stable are real
    bool     have_expr = false;   // gains and integration time are real
    uint32_t luma = 0;           // current scene luma as seen by AE
    uint32_t target = 0;         // AE target luma
    bool     stable = false;     // AE converged
    uint32_t total_gain_db = 0;  // sensor+ISP gain
    uint32_t exposure_value = 0; // integration x again x dgain
    uint32_t integration_time = 0;
    uint32_t again = 0, dgain = 0, isp_dgain = 0;
    // "Is the shutter at its AE ceiling?" -- answered here, against the
    // driver's own limits, because no browser-side threshold is portable
    // (the WebUI's video-check says exactly this, and without the answer it
    // convicts from the picture alone: a stalled player reads as a blind
    // camera). have_ guards it for the same reason isp_exptime is guarded
    // above: a false "not at max" nobody measured is a statement, not a gap.
    bool     have_exposure_max = false;
    bool     exposure_is_max = false;
};

class IImageControl {
public:
    virtual ~IImageControl() = default;
    virtual ImageCaps caps() const = 0;
    // Live set; `effective` read back from the ISP. Busy when tuning is not up.
    virtual Result set(ImageControl c, int value, int& effective) = 0;
    virtual Result get(ImageControl c, int& value) = 0;
    virtual Result exposure(ExposureReadback& out) = 0;
};

} // namespace machino
