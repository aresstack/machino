#include "adapters/ingenic/ingenic_jpeg.hpp"
#include "core/log.hpp"
#include <cstring>
#include <imp/imp_common.h>
#include <imp/imp_encoder.h>
#include <imp/imp_framesource.h>
#include <imp/imp_system.h>

namespace machino { namespace ingenic {

static const char* MOD = "ING_JPEG";

std::unique_ptr<IngenicJpegEncoder> IngenicJpegEncoder::create(int chn, const JpegParams& p, int native_w, int native_h) {
    int w = p.width > 0 ? p.width : native_w;
    int h = p.height > 0 ? p.height : native_h;
    if (w <= 0 || h <= 0) { LOGE(MOD, "jpeg: no geometry (w=%d h=%d)", w, h); return nullptr; }

    // Own FrameSource channel (scaled from the sensor when smaller than native).
    IMPFSChnAttr fa; memset(&fa, 0, sizeof fa);
    fa.pixFmt        = PIX_FMT_NV12;
    fa.outFrmRateNum = 15;             // snapshots do not need full rate; the encoder is ephemeral
    fa.outFrmRateDen = 1;
    fa.nrVBs         = p.buffers > 0 ? p.buffers : 1;
    fa.type          = FS_PHY_CHANNEL;
    fa.picWidth      = w;
    fa.picHeight     = h;
    if (w != native_w || h != native_h) {
        fa.scaler.enable    = 1;
        fa.scaler.outwidth  = w;
        fa.scaler.outheight = h;
    }
    auto fs = std::make_unique<imp::FrameSourceChannel>(chn, fa);
    if (!fs->ok()) { LOGE(MOD, "jpeg framesource chn%d failed (%d)", chn, fs->rc()); return nullptr; }

    // JPEG encoder channel. SetDefaultParam configures the JPEG profile; this
    // SDK (T40 1.3.1) has no SetJpegeQl, so the default quantisation is used -
    // p.quality is accepted but not yet mapped to an SDK control here.
    IMPEncoderChnAttr a; memset(&a, 0, sizeof a);
    int r = IMP_Encoder_SetDefaultParam(&a, IMP_ENC_PROFILE_JPEG, IMP_ENC_RC_MODE_FIXQP,
                                        (uint16_t)w, (uint16_t)h, 15, 1, 1, 1, -1, 0);
    if (r != 0) LOGW(MOD, "IMP_Encoder_SetDefaultParam(jpeg chn%d) returned %d - attr may be incomplete", chn, r);

    auto group = std::make_unique<imp::EncoderGroup>(chn);
    if (!group->ok()) { LOGE(MOD, "jpeg encoder group %d failed (%d)", chn, group->rc()); return nullptr; }
    auto channel = std::make_unique<imp::EncoderChannel>(chn, chn, a);
    if (!channel->ok()) { LOGE(MOD, "jpeg encoder chn%d failed (%d)", chn, channel->rc()); return nullptr; }

    IMPCell src = { DEV_ID_FS,  chn, 0 };
    IMPCell dst = { DEV_ID_ENC, chn, 0 };
    auto bind = std::make_unique<imp::Binding>(src, dst);
    if (!bind->ok()) { LOGE(MOD, "jpeg bind FS%d->ENC%d failed (%d)", chn, chn, bind->rc()); return nullptr; }

    Result er = fs->enable();
    if (!er) { LOGE(MOD, "jpeg framesource enable chn%d failed", chn); return nullptr; }

    auto self = std::unique_ptr<IngenicJpegEncoder>(
        new IngenicJpegEncoder(chn, std::move(fs), std::move(group), std::move(channel), std::move(bind)));
    r = IMP_Encoder_StartRecvPic(chn);
    if (r != 0) { LOGE(MOD, "IMP_Encoder_StartRecvPic(jpeg %d) failed (%d)", chn, r); return nullptr; }
    self->recv_ = true;
    LOGI(MOD, "jpeg chn%d %dx%d ready (scaler=%d)", chn, w, h, fa.scaler.enable);
    return self;
}

IngenicJpegEncoder::~IngenicJpegEncoder() {
    if (recv_) IMP_Encoder_StopRecvPic(chn_);
    // bind_ / chan_ / group_ / fs_ RAII teardown in declared order.
}

Result IngenicJpegEncoder::capture(std::vector<uint8_t>& out, int timeout_ms) {
    std::lock_guard<std::mutex> lk(sdk_m_);
    if (!recv_) return Result::busy();
    if (IMP_Encoder_PollingStream(chn_, (uint32_t)timeout_ms) != 0) return Result::timeout();

    IMPEncoderStream st; memset(&st, 0, sizeof st);
    int r = IMP_Encoder_GetStream(chn_, &st, 1);
    if (r != 0) { LOGW(MOD, "IMP_Encoder_GetStream(jpeg %d) failed (%d)", chn_, r); return Result::error(r); }

    // A JPEG frame is one complete JFIF image; copy the pack bytes verbatim -
    // no Annex-B start codes (that is the H.264 path).
    out.clear();
    const uint8_t* base = (const uint8_t*)(uintptr_t)st.virAddr;
    for (uint32_t i = 0; i < st.packCount; ++i) {
        const IMPEncoderPack& pk = st.pack[i];
        if (!pk.length) continue;
        const uint8_t* p = base + pk.offset;
        out.insert(out.end(), p, p + pk.length);
    }
    IMP_Encoder_ReleaseStream(chn_, &st);
    if (out.size() < 2 || out.front() != 0xFF) { LOGW(MOD, "jpeg chn%d: implausible frame (%zu bytes)", chn_, out.size()); return Result::error(-1); }
    return Result::ok();
}

}} // namespace machino::ingenic
