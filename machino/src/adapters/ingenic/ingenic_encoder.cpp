#include "adapters/ingenic/ingenic_encoder.hpp"
#include "core/log.hpp"
#include <cstring>

namespace machino { namespace ingenic {

static const char* MOD = "ING_ENC";

std::unique_ptr<IngenicEncoder> IngenicEncoder::create(int chn, const EffectiveStream& sc) {
    IMPEncoderChnAttr a; memset(&a, 0, sizeof a);
    IMPEncoderProfile prof = sc.profile >= 2 ? IMP_ENC_PROFILE_AVC_HIGH
                           : sc.profile == 1 ? IMP_ENC_PROFILE_AVC_MAIN
                                             : IMP_ENC_PROFILE_AVC_BASELINE;
    IMPEncoderRcMode rc = sc.rc == RcMode::Vbr   ? IMP_ENC_RC_MODE_VBR
                        : sc.rc == RcMode::FixQp ? IMP_ENC_RC_MODE_FIXQP
                                                 : IMP_ENC_RC_MODE_CBR;
    int initial_qp = (sc.rc == RcMode::FixQp) ? sc.qp : -1;
    int r = IMP_Encoder_SetDefaultParam(&a, prof, rc, (uint16_t)sc.width, (uint16_t)sc.height,
                                        (uint32_t)sc.fps, 1, (uint32_t)sc.gop, 1, initial_qp,
                                        (uint32_t)sc.bitrate_kbps);
    if (r != 0) LOGW(MOD, "IMP_Encoder_SetDefaultParam(chn%d) returned %d - attr may be incomplete", chn, r);
    a.gopAttr.uGopLength       = (uint16_t)sc.gop;
    a.gopAttr.uMaxSameSenceCnt = 1;

    auto group = std::make_unique<imp::EncoderGroup>(chn);
    if (!group->ok()) return nullptr;
    auto channel = std::make_unique<imp::EncoderChannel>(chn, chn, a);
    if (!channel->ok()) return nullptr;

    LOGI(MOD, "chn%d h264 %s %dx%d@%d gop=%d %s %dkbps", chn,
         sc.profile >= 2 ? "high" : sc.profile == 1 ? "main" : "baseline",
         sc.width, sc.height, sc.fps, sc.gop,
         sc.rc == RcMode::Vbr ? "vbr" : sc.rc == RcMode::FixQp ? "fixqp" : "cbr", sc.bitrate_kbps);
    return std::unique_ptr<IngenicEncoder>(new IngenicEncoder(std::move(group), std::move(channel), sc.rc));
}

Result IngenicEncoder::start() {
    if (rx_) return Result::ok();
    auto rx = std::make_unique<imp::StreamReceiver>(chan_->chn());
    if (!rx->ok()) return Result::error(rx->rc());
    rx_ = std::move(rx);
    return Result::ok();
}

Result IngenicEncoder::stop() { rx_.reset(); return Result::ok(); }

Result IngenicEncoder::fetch(AccessUnit& out, int timeout_ms) {
    if (!rx_) return Result::busy();
    return rx_->fetch(out, timeout_ms);
}

// Live bitrate: read the current RC attributes, change only the target bit
// rate of the active mode, write back, read back the effective value.
Result IngenicEncoder::set_bitrate(int kbps, int& effective) {
    effective = -1;
    if (rc_ == RcMode::FixQp) return Result::unsupported();     // no bitrate in fixed-QP mode
    IMPEncoderAttrRcMode rc; memset(&rc, 0, sizeof rc);
    int r = IMP_Encoder_GetChnAttrRcMode(chan_->chn(), &rc);
    if (r != 0) { LOGW(MOD, "IMP_Encoder_GetChnAttrRcMode(%d) failed (%d)", chan_->chn(), r); return Result::error(r); }
    switch (rc.rcMode) {
        case IMP_ENC_RC_MODE_CBR:            rc.attrCbr.uTargetBitRate = (uint32_t)kbps; break;
        case IMP_ENC_RC_MODE_VBR:            rc.attrVbr.uTargetBitRate = (uint32_t)kbps; if (rc.attrVbr.uMaxBitRate < (uint32_t)kbps) rc.attrVbr.uMaxBitRate = (uint32_t)kbps; break;
        case IMP_ENC_RC_MODE_CAPPED_VBR:
        case IMP_ENC_RC_MODE_CAPPED_QUALITY: rc.attrCappedVbr.uTargetBitRate = (uint32_t)kbps; if (rc.attrCappedVbr.uMaxBitRate < (uint32_t)kbps) rc.attrCappedVbr.uMaxBitRate = (uint32_t)kbps; break;
        default: return Result::unsupported();
    }
    r = IMP_Encoder_SetChnAttrRcMode(chan_->chn(), &rc);
    if (r != 0) { LOGW(MOD, "IMP_Encoder_SetChnAttrRcMode(%d, %d kbps) failed (%d)", chan_->chn(), kbps, r); return Result::error(r); }
    IMPEncoderAttrRcMode back; memset(&back, 0, sizeof back);
    if (IMP_Encoder_GetChnAttrRcMode(chan_->chn(), &back) == 0) {
        effective = (int)(back.rcMode == IMP_ENC_RC_MODE_CBR ? back.attrCbr.uTargetBitRate
                        : back.rcMode == IMP_ENC_RC_MODE_VBR ? back.attrVbr.uTargetBitRate
                        : back.attrCappedVbr.uTargetBitRate);
    }
    LOGI(MOD, "chn%d bitrate live: requested=%d effective=%d", chan_->chn(), kbps, effective);
    return Result::ok();
}

// Encoder-side frame rate (takes effect in the next GOP per SDK). This alone
// does NOT lower the sensor/ISP rate - the service accounts for that.
Result IngenicEncoder::set_fps(int fps, int& effective) {
    effective = -1;
    IMPEncoderFrmRate fr; fr.frmRateNum = (uint32_t)fps; fr.frmRateDen = 1;
    int r = IMP_Encoder_SetChnFrmRate(chan_->chn(), &fr);
    if (r != 0) { LOGW(MOD, "IMP_Encoder_SetChnFrmRate(%d, %d) failed (%d)", chan_->chn(), fps, r); return Result::error(r); }
    IMPEncoderFrmRate back; memset(&back, 0, sizeof back);
    if (IMP_Encoder_GetChnFrmRate(chan_->chn(), &back) == 0 && back.frmRateDen > 0) effective = (int)(back.frmRateNum / back.frmRateDen);
    LOGI(MOD, "chn%d encoder fps live: requested=%d effective=%d", chan_->chn(), fps, effective);
    return Result::ok();
}

}} // namespace machino::ingenic
