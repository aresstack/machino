#include "adapters/ingenic/imp_sessions.hpp"
#include "core/log.hpp"
#include <cstring>
#include <unistd.h>

namespace machino { namespace ingenic { namespace imp {

static const char* MOD = "IMP";

// ---- IspSession -----------------------------------------------------------
IspSession::IspSession() {
    rc_ = IMP_ISP_Open();
    ok_ = rc_ >= 0;
    if (!ok_) LOGE(MOD, "IMP_ISP_Open failed (%d)", rc_);
}
IspSession::~IspSession() { if (ok_) IMP_ISP_Close(); }

// ---- SensorSession --------------------------------------------------------
SensorSession::SensorSession(const IMPSensorInfo& info) : info_(info) {
    rc_ = IMP_ISP_AddSensor(IMPVI_MAIN, &info_);
    if (rc_ < 0) { LOGE(MOD, "IMP_ISP_AddSensor(%s i2c%d/0x%02x mclk%d rst=%d pwdn=%d) failed (%d)",
                        info_.name, info_.i2c.i2c_adapter_id, info_.i2c.addr, (int)info_.mclk,
                        info_.rst_gpio, info_.pwdn_gpio, rc_); return; }
    added_ = true;
    rc_ = IMP_ISP_EnableSensor(IMPVI_MAIN, &info_);
    if (rc_ < 0) { LOGE(MOD, "IMP_ISP_EnableSensor failed (%d)", rc_); return; }
    enabled_ = true;
}
SensorSession::~SensorSession() {
    if (enabled_) IMP_ISP_DisableSensor(IMPVI_MAIN);
    if (added_)   IMP_ISP_DelSensor(IMPVI_MAIN, &info_);
}

// ---- SystemSession --------------------------------------------------------
SystemSession::SystemSession(int retries, int retry_delay_ms) {
    for (int t = 0; ; ++t) {
        rc_ = IMP_System_Init();
        if (rc_ >= 0) { ok_ = true; return; }
        if (t + 1 >= retries) { LOGE(MOD, "IMP_System_Init failed (%d) after %d tries", rc_, t + 1); return; }
        LOGW(MOD, "IMP_System_Init failed (%d) - retry %d", rc_, t + 1);
        usleep((useconds_t)retry_delay_ms * 1000);
    }
}
SystemSession::~SystemSession() { if (ok_) IMP_System_Exit(); }

// ---- TuningSession --------------------------------------------------------
TuningSession::TuningSession() {
    int rc = IMP_ISP_EnableTuning();
    ok_ = rc >= 0;
    if (!ok_) LOGW(MOD, "IMP_ISP_EnableTuning failed (%d) - continuing without tuning", rc);
}
TuningSession::~TuningSession() { if (ok_) IMP_ISP_DisableTuning(); }

// ---- FrameSourceChannel ---------------------------------------------------
FrameSourceChannel::FrameSourceChannel(int chn, const IMPFSChnAttr& attr) : chn_(chn) {
    rc_ = IMP_FrameSource_CreateChn(chn_, const_cast<IMPFSChnAttr*>(&attr));
    if (rc_ < 0) { LOGE(MOD, "IMP_FrameSource_CreateChn(%d) failed (%d)", chn_, rc_); return; }
    rc_ = IMP_FrameSource_SetChnAttr(chn_, const_cast<IMPFSChnAttr*>(&attr));
    if (rc_ < 0) { LOGE(MOD, "IMP_FrameSource_SetChnAttr(%d) failed (%d)", chn_, rc_); IMP_FrameSource_DestroyChn(chn_); return; }
    ok_ = true;
}
FrameSourceChannel::~FrameSourceChannel() {
    if (!ok_) return;
    if (enabled_) IMP_FrameSource_DisableChn(chn_);
    IMP_FrameSource_DestroyChn(chn_);
}
Result FrameSourceChannel::enable() {
    if (!ok_) return Result::busy();
    if (enabled_) return Result::ok();
    int rc = IMP_FrameSource_EnableChn(chn_);
    if (rc != 0) { LOGE(MOD, "IMP_FrameSource_EnableChn(%d) failed (%d)", chn_, rc); return Result::error(rc); }
    enabled_ = true; return Result::ok();
}
Result FrameSourceChannel::disable() {
    if (!enabled_) return Result::ok();
    int rc = IMP_FrameSource_DisableChn(chn_);
    enabled_ = false;
    if (rc != 0) { LOGW(MOD, "IMP_FrameSource_DisableChn(%d) failed (%d)", chn_, rc); return Result::error(rc); }
    return Result::ok();
}

// ---- EncoderGroup ---------------------------------------------------------
EncoderGroup::EncoderGroup(int grp) : grp_(grp) {
    rc_ = IMP_Encoder_CreateGroup(grp_);
    ok_ = rc_ >= 0;
    if (!ok_) LOGE(MOD, "IMP_Encoder_CreateGroup(%d) failed (%d)", grp_, rc_);
}
EncoderGroup::~EncoderGroup() { if (ok_) IMP_Encoder_DestroyGroup(grp_); }

// ---- EncoderChannel -------------------------------------------------------
EncoderChannel::EncoderChannel(int chn, int grp, const IMPEncoderChnAttr& attr) : chn_(chn) {
    rc_ = IMP_Encoder_CreateChn(chn_, &attr);
    if (rc_ < 0) { LOGE(MOD, "IMP_Encoder_CreateChn(%d) failed (%d)", chn_, rc_); return; }
    created_ = true;
    rc_ = IMP_Encoder_RegisterChn(grp, chn_);
    if (rc_ != 0) { LOGE(MOD, "IMP_Encoder_RegisterChn(%d,%d) failed (%d)", grp, chn_, rc_); return; }
    registered_ = true;
}
EncoderChannel::~EncoderChannel() {
    if (registered_) IMP_Encoder_UnRegisterChn(chn_);
    if (created_)    IMP_Encoder_DestroyChn(chn_);
}

// ---- Binding --------------------------------------------------------------
Binding::Binding(IMPCell src, IMPCell dst) : src_(src), dst_(dst) {
    rc_ = IMP_System_Bind(&src_, &dst_);
    ok_ = rc_ >= 0;
    if (!ok_) LOGE(MOD, "IMP_System_Bind(%d.%d -> %d.%d) failed (%d)", (int)src_.deviceID, src_.groupID, (int)dst_.deviceID, dst_.groupID, rc_);
}
Binding::~Binding() { if (ok_) IMP_System_UnBind(&src_, &dst_); }

// ---- StreamReceiver -------------------------------------------------------
static bool annexb_has_idr(const uint8_t* p, size_t n) {
    for (size_t i = 0; i + 4 <= n; ++i) {
        if (p[i] == 0 && p[i+1] == 0 && p[i+2] == 1) {
            if (i + 3 < n && (p[i+3] & 0x1f) == 5) return true;
            i += 2;
        }
    }
    return false;
}

StreamReceiver::StreamReceiver(int chn) : chn_(chn) {
    rc_ = IMP_Encoder_StartRecvPic(chn_);
    ok_ = rc_ == 0;
    if (!ok_) LOGE(MOD, "IMP_Encoder_StartRecvPic(%d) failed (%d)", chn_, rc_);
}
StreamReceiver::~StreamReceiver() { if (ok_) IMP_Encoder_StopRecvPic(chn_); }

// IMP_Encoder is not thread-safe across threads: RequestIDR is issued from the
// fetching thread only (the flag may be set from anywhere).
Result StreamReceiver::fetch(AccessUnit& out, int timeout_ms) {
    if (!ok_) return Result::busy();
    if (idr_pending_) { idr_pending_ = false; IMP_Encoder_RequestIDR(chn_); }
    if (IMP_Encoder_PollingStream(chn_, (uint32_t)timeout_ms) != 0) return Result::timeout();

    IMPEncoderStream st; memset(&st, 0, sizeof st);
    int r = IMP_Encoder_GetStream(chn_, &st, 1);
    if (r != 0) { LOGW(MOD, "IMP_Encoder_GetStream(%d) failed (%d)", chn_, r); return Result::error(r); }

    out.data.clear();                                   // keeps capacity: no per-frame allocation once warm
    const uint8_t* base = (const uint8_t*)(uintptr_t)st.virAddr;
    static const uint8_t sc[4] = {0, 0, 0, 1};
    for (uint32_t i = 0; i < st.packCount; ++i) {
        const IMPEncoderPack& pk = st.pack[i];
        if (!pk.length) continue;
        const uint8_t* p = base + pk.offset; size_t l = pk.length;
        bool has_sc = l >= 3 && p[0] == 0 && p[1] == 0 && (p[2] == 1 || (l >= 4 && p[2] == 0 && p[3] == 1));
        if (!has_sc) out.data.insert(out.data.end(), sc, sc + 4);
        out.data.insert(out.data.end(), p, p + l);
    }
    out.pts_us = st.packCount ? st.pack[0].timestamp : 0;
    out.key    = annexb_has_idr(out.data.data(), out.data.size());
    IMP_Encoder_ReleaseStream(chn_, &st);
    return Result::ok();
}

}}} // namespace machino::ingenic::imp
