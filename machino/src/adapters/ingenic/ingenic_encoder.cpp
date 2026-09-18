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

    auto group = std::make_unique<imp::EncoderGroup>(chn);          // group id == channel id
    if (!group->ok()) return nullptr;
    auto channel = std::make_unique<imp::EncoderChannel>(chn, chn, a);
    if (!channel->ok()) return nullptr;                             // group rolls back

    LOGI(MOD, "chn%d h264 %s %dx%d@%d gop=%d %s %dkbps", chn,
         sc.profile >= 2 ? "high" : sc.profile == 1 ? "main" : "baseline",
         sc.width, sc.height, sc.fps, sc.gop,
         sc.rc == RcMode::Vbr ? "vbr" : sc.rc == RcMode::FixQp ? "fixqp" : "cbr", sc.bitrate_kbps);
    return std::unique_ptr<IngenicEncoder>(new IngenicEncoder(std::move(group), std::move(channel)));
}

Result IngenicEncoder::start() {
    if (rx_) return Result::ok();
    auto rx = std::make_unique<imp::StreamReceiver>(chan_->chn());
    if (!rx->ok()) return Result::error(rx->rc());
    rx_ = std::move(rx);
    return Result::ok();
}

Result IngenicEncoder::stop() {
    rx_.reset();
    return Result::ok();
}

Result IngenicEncoder::fetch(AccessUnit& out, int timeout_ms) {
    if (!rx_) return Result::busy();
    return rx_->fetch(out, timeout_ms);
}

}} // namespace machino::ingenic
