#include "adapters/ingenic/imp_sessions.hpp"
#include "core/log.hpp"
#include <cstddef>
#include <cstring>
#include <unistd.h>

namespace machino { namespace ingenic { namespace imp {

// AP22: a compile-time gate on the one vendor struct this file reads through a
// raw pointer.
//
// IMP_Encoder_GetStream fills an IMPEncoderStream, and fetch() below walks
// st.pack[0..packCount) using st.virAddr as the base. Every one of those is an
// offset into a struct whose layout comes from a header that is NOT versioned
// with the library it talks to: Machino links libimp 1.3.1 statically while the
// camera carries 1.2.0 in /usr/lib, and the headers are a separate submodule
// that can be repointed on its own. If the two ever disagree about this layout
// the failure is not a compile error - it is reading a length and a pointer
// from the wrong words and walking off into memory.
//
// So the layout is asserted here rather than trusted. These are not arbitrary
// numbers to keep green: if a new SDK genuinely changes the struct, this must
// FAIL, be read, and the reader below adjusted deliberately - that is the whole
// point. The values are the T40 1.3.1 header, 32-bit MIPS o32.

// The premise the sizes below rest on. Only the cross build compiles this file
// and it is 32-bit o32 - spelling that out here means a future 64-bit target
// fails on THIS line, where the reason is written down, instead of on a size
// that would look arbitrary.
static_assert(sizeof(void*) == 4, "these layouts assume 32-bit pointers (T40 o32)");

// Pack is 32, not 28: frameEnd is a bool at offset 16, the two enums follow at
// 20 and 24, and the int64_t timestamp gives the struct 8-byte alignment, so it
// is padded out. Computed with the real header rather than counted by hand -
// the hand count said 24 and was wrong.
static_assert(sizeof(IMPEncoderPack) == 32, "IMPEncoderPack layout changed - re-read fetch()");
static_assert(offsetof(IMPEncoderPack, offset) == 0, "IMPEncoderPack::offset moved");
static_assert(offsetof(IMPEncoderPack, length) == 4, "IMPEncoderPack::length moved");
static_assert(offsetof(IMPEncoderPack, timestamp) == 8, "IMPEncoderPack::timestamp moved");
static_assert(sizeof(IMPEncoderStream) == 28, "IMPEncoderStream layout changed - re-read fetch()");
static_assert(offsetof(IMPEncoderStream, virAddr) == 4, "IMPEncoderStream::virAddr moved");
static_assert(offsetof(IMPEncoderStream, pack) == 12, "IMPEncoderStream::pack moved");
static_assert(offsetof(IMPEncoderStream, packCount) == 16, "IMPEncoderStream::packCount moved");

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
SystemSession::SystemSession() {
    rc_ = IMP_System_Init();
    if (rc_ >= 0) { ok_ = true; return; }
    // One attempt, then give up - see the header for why the five-retry loop
    // had to go. The caller records the stage and moves to FAILED.
    LOGE(MOD, "IMP_System_Init failed (%d) - not retrying", rc_);
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
    if (idr_pending_.exchange(false, std::memory_order_acq_rel)) IMP_Encoder_RequestIDR(chn_);
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
