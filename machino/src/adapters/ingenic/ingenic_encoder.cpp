#include "adapters/ingenic/ingenic_encoder.hpp"
#include "core/log.hpp"

#include <imp/imp_encoder.h>
#include <cstring>

namespace machino { namespace ingenic {

static const char* MOD = "ING_ENC";

// Platform-neutral key detection: scan Annex-B for an IDR slice (type 5).
static bool annexb_has_idr(const uint8_t* p, size_t n) {
    for (size_t i = 0; i + 4 <= n; ++i) {
        if (p[i] == 0 && p[i+1] == 0 && p[i+2] == 1) {
            if (i + 3 < n && (p[i+3] & 0x1f) == 5) return true;
            i += 2;
        }
    }
    return false;
}

std::unique_ptr<IngenicEncoder> IngenicEncoder::create(int chn, const StreamConfig& sc) {
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

    std::unique_ptr<IngenicEncoder> e(new IngenicEncoder(chn));
    int grp = chn;
    r = IMP_Encoder_CreateGroup(grp);
    if (r < 0) { LOGE(MOD, "IMP_Encoder_CreateGroup(%d) failed (%d)", grp, r); return nullptr; }
    e->group_ = true;

    r = IMP_Encoder_CreateChn(chn, &a);
    if (r < 0) { LOGE(MOD, "IMP_Encoder_CreateChn(%d) failed (%d)", chn, r); return nullptr; }
    e->channel_ = true;

    r = IMP_Encoder_RegisterChn(grp, chn);
    if (r != 0) { LOGE(MOD, "IMP_Encoder_RegisterChn(%d,%d) failed (%d)", grp, chn, r); return nullptr; }
    e->registered_ = true;

    LOGI(MOD, "chn%d created h264 %s %dx%d@%d gop=%d %s %dkbps", chn,
         sc.profile >= 2 ? "high" : sc.profile == 1 ? "main" : "baseline",
         sc.width, sc.height, sc.fps, sc.gop,
         sc.rc == RcMode::Vbr ? "vbr" : sc.rc == RcMode::FixQp ? "fixqp" : "cbr", sc.bitrate_kbps);
    return e;
}

IngenicEncoder::~IngenicEncoder() {
    if (receiving_)  IMP_Encoder_StopRecvPic(chn_);
    if (registered_) IMP_Encoder_UnRegisterChn(chn_);
    if (channel_)    IMP_Encoder_DestroyChn(chn_);
    if (group_)      IMP_Encoder_DestroyGroup(chn_);
    LOGD(MOD, "chn%d destroyed", chn_);
}

Result IngenicEncoder::start() {
    if (receiving_) return Result::ok();
    int r = IMP_Encoder_StartRecvPic(chn_);
    if (r != 0) { LOGE(MOD, "IMP_Encoder_StartRecvPic(%d) failed (%d)", chn_, r); return Result::error(r); }
    receiving_ = true;
    idr_pending_ = true;
    return Result::ok();
}

Result IngenicEncoder::stop() {
    if (!receiving_) return Result::ok();
    int r = IMP_Encoder_StopRecvPic(chn_);
    receiving_ = false;
    if (r != 0) { LOGW(MOD, "IMP_Encoder_StopRecvPic(%d) failed (%d)", chn_, r); return Result::error(r); }
    return Result::ok();
}

void IngenicEncoder::request_idr() { idr_pending_ = true; }

// IMP_Encoder is not thread-safe across threads: RequestIDR is issued from
// the fetching thread only (the flag is set by anyone).
Result IngenicEncoder::fetch(AccessUnit& out, int timeout_ms) {
    if (!receiving_) return Result::busy();
    if (idr_pending_) { idr_pending_ = false; IMP_Encoder_RequestIDR(chn_); }

    if (IMP_Encoder_PollingStream(chn_, (uint32_t)timeout_ms) != 0) return Result::timeout();

    IMPEncoderStream st; memset(&st, 0, sizeof st);
    int r = IMP_Encoder_GetStream(chn_, &st, 1);
    if (r != 0) { LOGW(MOD, "IMP_Encoder_GetStream(%d) failed (%d)", chn_, r); return Result::error(r); }

    size_t need = 0;
    for (uint32_t i = 0; i < st.packCount; ++i) if (st.pack[i].length) need += st.pack[i].length + 4;
    out.data.clear();
    out.data.reserve(need);
    const uint8_t* base = (const uint8_t*)(uintptr_t)st.virAddr;
    for (uint32_t i = 0; i < st.packCount; ++i) {
        const IMPEncoderPack& pk = st.pack[i];
        if (!pk.length) continue;
        const uint8_t* p = base + pk.offset;
        size_t l = pk.length;
        bool has_sc = l >= 3 && p[0] == 0 && p[1] == 0 && (p[2] == 1 || (l >= 4 && p[2] == 0 && p[3] == 1));
        if (!has_sc) { static const uint8_t sc[4] = {0, 0, 0, 1}; out.data.insert(out.data.end(), sc, sc + 4); }
        out.data.insert(out.data.end(), p, p + l);
    }
    out.pts_us = st.packCount ? st.pack[0].timestamp : 0;
    out.key    = annexb_has_idr(out.data.data(), out.data.size());
    IMP_Encoder_ReleaseStream(chn_, &st);
    return Result::ok();
}

}} // namespace machino::ingenic
