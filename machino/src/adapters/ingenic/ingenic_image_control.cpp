#include "adapters/ingenic/ingenic_image_control.hpp"
#include "core/log.hpp"
#include <cstring>
#include <imp/imp_isp.h>

namespace machino { namespace ingenic {

static const char* MOD = "ING_IMG";

ImageCaps IngenicImageControl::caps() const {
    ImageCaps c{};
    auto sup = [&](ImageControl k, int lo, int hi) { c.control[(int)k] = RangeCap{Cap::Supported, lo, hi, ApplyMode::Live}; };
    sup(ImageControl::Brightness, 0, 255); sup(ImageControl::Contrast, 0, 255);
    sup(ImageControl::Saturation, 0, 255); sup(ImageControl::Sharpness, 0, 255);
    sup(ImageControl::Hue, 0, 255);
    sup(ImageControl::HFlip, 0, 1); sup(ImageControl::VFlip, 0, 1);
    sup(ImageControl::AntiFlicker, 0, 60);                       // 0 | 50 | 60
    sup(ImageControl::AeCompensation, 0, 255);                   // AeTargetComp; 0 = disabled
    sup(ImageControl::HighlightDepress, 0, 10); sup(ImageControl::BacklightComp, 0, 10);
    sup(ImageControl::WhiteBalanceMode, 0, 9);                 // public IMPISPAWBMode; auto remains the default
    sup(ImageControl::RunningMode, 0, 1);
    sup(ImageControl::TemporalNr, 0, 1); sup(ImageControl::SpatialNr, 0, 1);
    sup(ImageControl::Dpc, 0, 1); sup(ImageControl::Defog, 0, 1);
    c.control[(int)ImageControl::Wdr] = RangeCap{Cap::Unsupported, -1, -1, ApplyMode::Unsupported};
    c.control[(int)ImageControl::Drc] = RangeCap{Cap::Unsupported, -1, -1, ApplyMode::Unsupported};
    return c;
}

static Result rc(int32_t r) { return r == 0 ? Result::ok() : Result::error((int)r); }

// simple unsigned char controls
static int32_t set_uc(ImageControl c, unsigned char v) {
    switch (c) {
        case ImageControl::Brightness: return IMP_ISP_Tuning_SetBrightness(IMPVI_MAIN, &v);
        case ImageControl::Contrast:   return IMP_ISP_Tuning_SetContrast(IMPVI_MAIN, &v);
        case ImageControl::Saturation: return IMP_ISP_Tuning_SetSaturation(IMPVI_MAIN, &v);
        case ImageControl::Sharpness:  return IMP_ISP_Tuning_SetSharpness(IMPVI_MAIN, &v);
        case ImageControl::Hue:        return IMP_ISP_Tuning_SetBcshHue(IMPVI_MAIN, &v);
        default: return -1;
    }
}
static int32_t get_uc(ImageControl c, unsigned char& v) {
    switch (c) {
        case ImageControl::Brightness: return IMP_ISP_Tuning_GetBrightness(IMPVI_MAIN, &v);
        case ImageControl::Contrast:   return IMP_ISP_Tuning_GetContrast(IMPVI_MAIN, &v);
        case ImageControl::Saturation: return IMP_ISP_Tuning_GetSaturation(IMPVI_MAIN, &v);
        case ImageControl::Sharpness:  return IMP_ISP_Tuning_GetSharpness(IMPVI_MAIN, &v);
        case ImageControl::Hue:        return IMP_ISP_Tuning_GetBcshHue(IMPVI_MAIN, &v);
        default: return -1;
    }
}

static bool is_uc(ImageControl c) {
    return c == ImageControl::Brightness || c == ImageControl::Contrast || c == ImageControl::Saturation ||
           c == ImageControl::Sharpness || c == ImageControl::Hue;
}

// ISP module bypass bits used for the NR/DPC/Defog toggles
static bool module_bit(ImageControl c, IMPISPModuleCtl& m, bool enable) {
    switch (c) {
        case ImageControl::TemporalNr: m.bitBypassMDNS = enable ? 0 : 1; return true;
        case ImageControl::SpatialNr:  m.bitBypassSDNS = enable ? 0 : 1; return true;
        case ImageControl::Dpc:        m.bitBypassDPC  = enable ? 0 : 1; return true;
        case ImageControl::Defog:      m.bitBypassDEFOG = enable ? 0 : 1; return true;
        default: return false;
    }
}
static int module_get(ImageControl c, const IMPISPModuleCtl& m) {
    switch (c) {
        case ImageControl::TemporalNr: return m.bitBypassMDNS ? 0 : 1;
        case ImageControl::SpatialNr:  return m.bitBypassSDNS ? 0 : 1;
        case ImageControl::Dpc:        return m.bitBypassDPC ? 0 : 1;
        case ImageControl::Defog:      return m.bitBypassDEFOG ? 0 : 1;
        default: return -1;
    }
}

Result IngenicImageControl::set(ImageControl c, int value, int& effective) {
    effective = -1;
    if (!active_) return Result::busy();
    int32_t r = 0;
    if (is_uc(c)) {
        r = set_uc(c, (unsigned char)value);
        if (r == 0) { unsigned char v = 0; if (get_uc(c, v) == 0) effective = v; }
    } else if (c == ImageControl::HFlip || c == ImageControl::VFlip) {
        IMPISPHVFLIP cur = IMPISP_FLIP_NORMAL_MODE;
        IMP_ISP_Tuning_GetHVFlip(IMPVI_MAIN, &cur);
        bool h = (cur == IMPISP_FLIP_H_MODE || cur == IMPISP_FLIP_HV_MODE), v = (cur == IMPISP_FLIP_V_MODE || cur == IMPISP_FLIP_HV_MODE);
        if (c == ImageControl::HFlip) h = value != 0; else v = value != 0;
        IMPISPHVFLIP m = h && v ? IMPISP_FLIP_HV_MODE : h ? IMPISP_FLIP_H_MODE : v ? IMPISP_FLIP_V_MODE : IMPISP_FLIP_NORMAL_MODE;
        r = IMP_ISP_Tuning_SetHVFLIP(IMPVI_MAIN, &m);
        if (r == 0) { int g; if (get(c, g)) effective = g; }
    } else if (c == ImageControl::AntiFlicker) {
        IMPISPAntiflickerAttr a; memset(&a, 0, sizeof a);
        if (value == 0) { a.mode = IMPISP_ANTIFLICKER_DISABLE_MODE; a.freq = 50; }
        else if (value == 50 || value == 60) { a.mode = IMPISP_ANTIFLICKER_NORMAL_MODE; a.freq = (uint8_t)value; }
        else return Result::unsupported();
        r = IMP_ISP_Tuning_SetAntiFlickerAttr(IMPVI_MAIN, &a);
        if (r == 0) { int g; if (get(c, g)) effective = g; }
    } else if (c == ImageControl::AeCompensation || c == ImageControl::HighlightDepress || c == ImageControl::BacklightComp) {
        IMPISPAEScenceAttr s; memset(&s, 0, sizeof s);
        r = IMP_ISP_Tuning_GetAeScenceAttr(IMPVI_MAIN, &s);
        if (r == 0) {
            if (c == ImageControl::AeCompensation) { s.AeTargetCompEn = value > 0 ? (IMPISPAEScenceMode)1 : (IMPISPAEScenceMode)0; s.AeTargetComp = (uint32_t)value; }
            else if (c == ImageControl::HighlightDepress) { s.AeHLCEn = value > 0 ? (IMPISPAEScenceMode)1 : (IMPISPAEScenceMode)0; s.AeHLCStrength = (unsigned char)value; }
            else { s.AeBLCEn = value > 0 ? (IMPISPAEScenceMode)1 : (IMPISPAEScenceMode)0; s.AeBLCStrength = (unsigned char)value; }
            r = IMP_ISP_Tuning_SetAeScenceAttr(IMPVI_MAIN, &s);
            if (r == 0) { int g; if (get(c, g)) effective = g; }
        }
    } else if (c == ImageControl::WhiteBalanceMode) {
        IMPISPWBAttr a; memset(&a, 0, sizeof a);
        r = IMP_ISP_Tuning_GetAwbAttr(IMPVI_MAIN, &a);
        if (r == 0) { a.mode = (IMPISPAWBMode)value; r = IMP_ISP_Tuning_SetAwbAttr(IMPVI_MAIN, &a); }
        if (r == 0) { int g; if (get(c, g)) effective = g; }
    } else if (c == ImageControl::RunningMode) {
        IMPISPRunningMode m = value ? IMPISP_RUNNING_MODE_NIGHT : IMPISP_RUNNING_MODE_DAY;
        r = IMP_ISP_Tuning_SetISPRunningMode(IMPVI_MAIN, &m);
        if (r == 0) { int g; if (get(c, g)) effective = g; }
    } else if (c == ImageControl::TemporalNr || c == ImageControl::SpatialNr || c == ImageControl::Dpc || c == ImageControl::Defog) {
        IMPISPModuleCtl m; memset(&m, 0, sizeof m);
        r = IMP_ISP_Tuning_GetModuleControl(IMPVI_MAIN, &m);
        if (r == 0) { module_bit(c, m, value != 0); r = IMP_ISP_Tuning_SetModuleControl(IMPVI_MAIN, &m); }
        if (r == 0) { int g; if (get(c, g)) effective = g; }
    } else return Result::unsupported();
    if (r != 0) { LOGW(MOD, "%s=%d failed (%d)", image_control_name(c), value, (int)r); return Result::error((int)r); }
    LOGI(MOD, "%s requested=%d effective=%d", image_control_name(c), value, effective);
    return Result::ok();
}

Result IngenicImageControl::get(ImageControl c, int& value) {
    value = -1;
    if (!active_) return Result::busy();
    if (is_uc(c)) { unsigned char v = 0; int32_t r = get_uc(c, v); if (r == 0) value = v; return rc(r); }
    if (c == ImageControl::HFlip || c == ImageControl::VFlip) {
        IMPISPHVFLIP cur = IMPISP_FLIP_NORMAL_MODE; int32_t r = IMP_ISP_Tuning_GetHVFlip(IMPVI_MAIN, &cur);
        if (r == 0) value = (c == ImageControl::HFlip) ? (cur == IMPISP_FLIP_H_MODE || cur == IMPISP_FLIP_HV_MODE) : (cur == IMPISP_FLIP_V_MODE || cur == IMPISP_FLIP_HV_MODE);
        return rc(r);
    }
    if (c == ImageControl::AntiFlicker) {
        IMPISPAntiflickerAttr a; memset(&a, 0, sizeof a); int32_t r = IMP_ISP_Tuning_GetAntiFlickerAttr(IMPVI_MAIN, &a);
        if (r == 0) value = (a.mode == IMPISP_ANTIFLICKER_DISABLE_MODE) ? 0 : a.freq;
        return rc(r);
    }
    if (c == ImageControl::AeCompensation || c == ImageControl::HighlightDepress || c == ImageControl::BacklightComp) {
        IMPISPAEScenceAttr s; memset(&s, 0, sizeof s); int32_t r = IMP_ISP_Tuning_GetAeScenceAttr(IMPVI_MAIN, &s);
        if (r == 0) value = c == ImageControl::AeCompensation ? (s.AeTargetCompEn ? (int)s.AeTargetComp : 0)
                          : c == ImageControl::HighlightDepress ? (s.AeHLCEn ? s.AeHLCStrength : 0) : (s.AeBLCEn ? s.AeBLCStrength : 0);
        return rc(r);
    }
    if (c == ImageControl::WhiteBalanceMode) {
        IMPISPWBAttr a; memset(&a, 0, sizeof a); int32_t r = IMP_ISP_Tuning_GetAwbAttr(IMPVI_MAIN, &a);
        if (r == 0) value = (int)a.mode;
        return rc(r);
    }
    if (c == ImageControl::RunningMode) {
        IMPISPRunningMode m = IMPISP_RUNNING_MODE_DAY; int32_t r = IMP_ISP_Tuning_GetISPRunningMode(IMPVI_MAIN, &m);
        if (r == 0) value = (m == IMPISP_RUNNING_MODE_NIGHT) ? 1 : 0;
        return rc(r);
    }
    if (c == ImageControl::TemporalNr || c == ImageControl::SpatialNr || c == ImageControl::Dpc || c == ImageControl::Defog) {
        IMPISPModuleCtl m; memset(&m, 0, sizeof m); int32_t r = IMP_ISP_Tuning_GetModuleControl(IMPVI_MAIN, &m);
        if (r == 0) value = module_get(c, m);
        return rc(r);
    }
    return Result::unsupported();
}

Result IngenicImageControl::exposure(ExposureReadback& out) {
    out = ExposureReadback{};
    if (!active_) return Result::busy();
    IMPISPAEScenceAttr s; memset(&s, 0, sizeof s);
    IMPISPAEExprInfo e; memset(&e, 0, sizeof e);
    int32_t r1 = IMP_ISP_Tuning_GetAeScenceAttr(IMPVI_MAIN, &s);
    int32_t r2 = IMP_ISP_Tuning_GetAeExprInfo(IMPVI_MAIN, &e);
    if (r1 != 0 && r2 != 0) return Result::error((int)r1);
    out.available = true;
    out.have_scene = (r1 == 0);
    out.have_expr  = (r2 == 0);
    if (r1 == 0) { out.luma = s.luma; out.target = s.target; out.stable = s.stable; }
    if (r2 == 0) { out.total_gain_db = e.TotalGainDb; out.exposure_value = e.ExposureValue; out.integration_time = e.AeShortIntegrationTime;
                   out.again = e.AeShortAGain; out.dgain = e.AeShortDGain; out.isp_dgain = e.AeShortIspDGain;
                   // Same field family the values above trust (Short carries
                   // the live numbers on this SoC); the plain pair is the
                   // fallback where a build leaves the Short limits at 0.
                   uint32_t it = e.AeShortIntegrationTime, itmax = e.AeShortMaxIntegrationTime;
                   if (itmax == 0) { it = e.AeIntegrationTime; itmax = e.AeMaxIntegrationTime; }
                   out.have_exposure_max = (itmax > 0);
                   out.exposure_is_max = (itmax > 0 && it >= itmax); }
    return Result::ok();
}

}} // namespace machino::ingenic
