#include "adapters/ingenic/ingenic_framesource.hpp"
#include "core/log.hpp"

#include <imp/imp_framesource.h>
#include <cstring>

namespace machino { namespace ingenic {

static const char* MOD = "ING_FS";

std::unique_ptr<IngenicFrameSource> IngenicFrameSource::create(int chn, const StreamConfig& sc,
                                                               const SensorConfig& sensor) {
    IMPFSChnAttr a; memset(&a, 0, sizeof a);
    a.pixFmt        = PIX_FMT_NV12;
    a.outFrmRateNum = sc.fps;
    a.outFrmRateDen = 1;
    a.nrVBs         = sc.buffers > 0 ? sc.buffers : 2;
    a.type          = FS_PHY_CHANNEL;
    a.picWidth      = sc.width;
    a.picHeight     = sc.height;
    if (sc.width != sensor.width || sc.height != sensor.height) {
        a.scaler.enable    = 1;
        a.scaler.outwidth  = sc.width;
        a.scaler.outheight = sc.height;
    }
    a.crop.enable = 0;
    a.fcrop.enable = 0;

    int rc = IMP_FrameSource_CreateChn(chn, &a);
    if (rc < 0) { LOGE(MOD, "IMP_FrameSource_CreateChn(%d) failed (%d)", chn, rc); return nullptr; }
    rc = IMP_FrameSource_SetChnAttr(chn, &a);
    if (rc < 0) {
        LOGE(MOD, "IMP_FrameSource_SetChnAttr(%d) failed (%d)", chn, rc);
        IMP_FrameSource_DestroyChn(chn);
        return nullptr;
    }
    LOGI(MOD, "chn%d created %dx%d@%d nv12 vbs=%d scaler=%d", chn, sc.width, sc.height, sc.fps,
         a.nrVBs, a.scaler.enable);
    return std::unique_ptr<IngenicFrameSource>(new IngenicFrameSource(chn));
}

IngenicFrameSource::~IngenicFrameSource() {
    if (enabled_) IMP_FrameSource_DisableChn(chn_);
    IMP_FrameSource_DestroyChn(chn_);
    LOGD(MOD, "chn%d destroyed", chn_);
}

Result IngenicFrameSource::enable() {
    if (enabled_) return Result::ok();
    int rc = IMP_FrameSource_EnableChn(chn_);
    if (rc != 0) { LOGE(MOD, "IMP_FrameSource_EnableChn(%d) failed (%d)", chn_, rc); return Result::error(rc); }
    enabled_ = true;
    return Result::ok();
}

Result IngenicFrameSource::disable() {
    if (!enabled_) return Result::ok();
    int rc = IMP_FrameSource_DisableChn(chn_);
    enabled_ = false;
    if (rc != 0) { LOGW(MOD, "IMP_FrameSource_DisableChn(%d) failed (%d)", chn_, rc); return Result::error(rc); }
    return Result::ok();
}

}} // namespace machino::ingenic
