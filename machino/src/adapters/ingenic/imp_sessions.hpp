// Ingenic adapter: RAII sessions over the IMP SDK. One class per vendor
// resource; the constructor acquires, the destructor releases, `ok()` tells
// whether acquisition succeeded. Building them into locals / unique_ptrs in
// bring-up order gives automatic reverse-order rollback on any failure
// without goto chains. The core never sees these types.
#pragma once
#include "core/frame.hpp"
#include "core/result.hpp"

#include <imp/imp_common.h>
#include <imp/imp_encoder.h>
#include <imp/imp_framesource.h>
#include <imp/imp_isp.h>
#include <imp/imp_system.h>
#include <atomic>

namespace machino { namespace ingenic { namespace imp {

class NonCopyable {
protected:
    NonCopyable() = default;
    ~NonCopyable() = default;
public:
    NonCopyable(const NonCopyable&) = delete;
    NonCopyable& operator=(const NonCopyable&) = delete;
};

// IMP_ISP_Open / IMP_ISP_Close
class IspSession : NonCopyable {
public:
    IspSession();
    ~IspSession();
    bool ok() const { return ok_; }
    int  rc() const { return rc_; }
private:
    bool ok_ = false; int rc_ = 0;
};

// IMP_ISP_AddSensor + IMP_ISP_EnableSensor  /  DisableSensor + DelSensor
class SensorSession : NonCopyable {
public:
    explicit SensorSession(const IMPSensorInfo& info);
    ~SensorSession();
    bool ok() const { return enabled_; }
    int  rc() const { return rc_; }
private:
    IMPSensorInfo info_;
    bool added_ = false, enabled_ = false; int rc_ = 0;
};

// IMP_System_Init (with retries) / IMP_System_Exit
class SystemSession : NonCopyable {
public:
    SystemSession(int retries, int retry_delay_ms);
    ~SystemSession();
    bool ok() const { return ok_; }
    int  rc() const { return rc_; }
private:
    bool ok_ = false; int rc_ = 0;
};

// IMP_ISP_EnableTuning / DisableTuning. Failure is non-fatal for M2.
class TuningSession : NonCopyable {
public:
    TuningSession();
    ~TuningSession();
    bool ok() const { return ok_; }
private:
    bool ok_ = false;
};

// IMP_FrameSource_CreateChn + SetChnAttr / DisableChn + DestroyChn
class FrameSourceChannel : NonCopyable {
public:
    FrameSourceChannel(int chn, const IMPFSChnAttr& attr);
    ~FrameSourceChannel();
    bool   ok() const { return ok_; }
    int    rc() const { return rc_; }
    int    chn() const { return chn_; }
    Result enable();
    Result disable();
private:
    int chn_; bool ok_ = false, enabled_ = false; int rc_ = 0;
};

// IMP_Encoder_CreateGroup / DestroyGroup
class EncoderGroup : NonCopyable {
public:
    explicit EncoderGroup(int grp);
    ~EncoderGroup();
    bool ok() const { return ok_; }
    int  rc() const { return rc_; }
    int  grp() const { return grp_; }
private:
    int grp_; bool ok_ = false; int rc_ = 0;
};

// IMP_Encoder_CreateChn + RegisterChn / UnRegisterChn + DestroyChn
class EncoderChannel : NonCopyable {
public:
    EncoderChannel(int chn, int grp, const IMPEncoderChnAttr& attr);
    ~EncoderChannel();
    bool ok() const { return registered_; }
    int  rc() const { return rc_; }
    int  chn() const { return chn_; }
private:
    int chn_; bool created_ = false, registered_ = false; int rc_ = 0;
};

// IMP_System_Bind / UnBind
class Binding : NonCopyable {
public:
    Binding(IMPCell src, IMPCell dst);
    ~Binding();
    bool ok() const { return ok_; }
    int  rc() const { return rc_; }
private:
    IMPCell src_, dst_; bool ok_ = false; int rc_ = 0;
};

// IMP_Encoder_StartRecvPic / StopRecvPic + the GetStream/ReleaseStream cycle.
class StreamReceiver : NonCopyable {
public:
    explicit StreamReceiver(int chn);
    ~StreamReceiver();
    bool   ok() const { return ok_; }
    int    rc() const { return rc_; }
    // Waits up to timeout_ms; fills `out` (reusing its capacity). Timeout when idle.
    Result fetch(AccessUnit& out, int timeout_ms);
    void   request_idr() { idr_pending_.store(true, std::memory_order_release); }
private:
    int chn_; bool ok_ = false; int rc_ = 0; std::atomic<bool> idr_pending_{true};
};

}}} // namespace machino::ingenic::imp
