#include "adapters/ingenic/detection/ivs_motion.hpp"
#include "adapters/ingenic/imp_sessions.hpp"
#include "core/detection/types.hpp"
#include "core/log.hpp"

#include <algorithm>
#include <cstring>
#include <memory>
#include <mutex>

#include <imp/imp_common.h>
#include <imp/imp_framesource.h>
#include <imp/imp_ivs.h>
#include <imp/imp_ivs_move.h>
#include <imp/imp_system.h>

namespace machino { namespace ingenic {

static const char* MOD = "ING_IVS";

namespace {

// The move algorithm runs on one IVS group/channel; only ever one detector
// (UNIT_AI) exists, so the well-trodden group 0 / channel 0 is used regardless
// of the analysis FrameSource channel number.
constexpr int IVS_GRP = 0;
constexpr int IVS_CHN = 0;

// A coarse motion grid: enough cells for a meaningful "level", well under the
// SDK's IMP_IVS_MOVE_MAX_ROI_CNT (52).
constexpr int GRID_COLS = 6;
constexpr int GRID_ROWS = 6;

// ---- IVS RAII sessions (local; the core never sees these) -------------------

// IMP_IVS_CreateGroup / DestroyGroup
class IvsGroup : imp::NonCopyable {
public:
    explicit IvsGroup(int grp) : grp_(grp) {
        rc_ = IMP_IVS_CreateGroup(grp_);
        ok_ = (rc_ == 0);
        if (!ok_) LOGE(MOD, "IMP_IVS_CreateGroup(%d) failed (%d)", grp_, rc_);
    }
    ~IvsGroup() { if (ok_) IMP_IVS_DestroyGroup(grp_); }
    bool ok() const { return ok_; }
    int  rc() const { return rc_; }
private:
    int grp_; bool ok_ = false; int rc_ = 0;
};

// IMP_IVS_CreateMoveInterface / DestroyMoveInterface
class IvsMoveIface : imp::NonCopyable {
public:
    explicit IvsMoveIface(IMP_IVS_MoveParam* param) {
        iface_ = IMP_IVS_CreateMoveInterface(param);
        if (!iface_) LOGE(MOD, "IMP_IVS_CreateMoveInterface failed");
    }
    ~IvsMoveIface() { if (iface_) IMP_IVS_DestroyMoveInterface(iface_); }
    bool ok() const { return iface_ != nullptr; }
    IMPIVSInterface* get() const { return iface_; }
private:
    IMPIVSInterface* iface_ = nullptr;
};

// IMP_IVS_CreateChn + RegisterChn / UnRegisterChn + DestroyChn
class IvsChannel : imp::NonCopyable {
public:
    IvsChannel(int grp, int chn, IMPIVSInterface* iface) : chn_(chn) {
        rc_ = IMP_IVS_CreateChn(chn_, iface);
        if (rc_ != 0) { LOGE(MOD, "IMP_IVS_CreateChn(%d) failed (%d)", chn_, rc_); return; }
        created_ = true;
        rc_ = IMP_IVS_RegisterChn(grp, chn_);
        if (rc_ != 0) { LOGE(MOD, "IMP_IVS_RegisterChn(%d,%d) failed (%d)", grp, chn_, rc_); return; }
        registered_ = true;
    }
    ~IvsChannel() {
        if (registered_) IMP_IVS_UnRegisterChn(chn_);
        if (created_)    IMP_IVS_DestroyChn(chn_);
    }
    bool ok() const { return registered_; }
    int  rc() const { return rc_; }
private:
    int chn_; bool created_ = false, registered_ = false; int rc_ = 0;
};

// ---- the detector -----------------------------------------------------------

class IngenicMotionDetector final : public IDetector {
public:
    IngenicMotionDetector(int fs_chn,
                          std::unique_ptr<IMP_IVS_MoveParam> mp,
                          std::unique_ptr<IvsGroup> group,
                          std::unique_ptr<imp::FrameSourceChannel> fs,
                          std::unique_ptr<imp::Binding> bind,
                          std::unique_ptr<IvsMoveIface> iface,
                          std::unique_ptr<IvsChannel> chan,
                          int roi_cnt)
        : fs_chn_(fs_chn), mp_(std::move(mp)), group_(std::move(group)), fs_(std::move(fs)), bind_(std::move(bind)),
          iface_(std::move(iface)), chan_(std::move(chan)), roi_cnt_(roi_cnt) {}

    ~IngenicMotionDetector() override { stop(); }

    DetectorInput input_mode() const override { return DetectorInput::BoundSource; }
    const char*   backend()    const override { return "imp_ivs_move"; }

    // Frames start flowing only now: enable the analysis channel, then let IVS
    // receive pictures. Order mirrors the SDK sample (streamon, then start).
    Result start() override {
        std::lock_guard<std::mutex> lk(m_);
        if (recv_) return Result::ok();
        Result er = fs_->enable();
        if (!er) return er;
        int rc = IMP_IVS_StartRecvPic(IVS_CHN);
        if (rc != 0) { LOGE(MOD, "IMP_IVS_StartRecvPic(%d) failed (%d)", IVS_CHN, rc); fs_->disable(); return Result::error(rc); }
        recv_ = true;
        LOGI(MOD, "motion detector active: fs chn%d -> ivs grp%d chn%d, %d cells", fs_chn_, IVS_GRP, IVS_CHN, roi_cnt_);
        return Result::ok();
    }

    Result stop() override {
        std::lock_guard<std::mutex> lk(m_);
        if (recv_) { IMP_IVS_StopRecvPic(IVS_CHN); recv_ = false; }
        if (fs_) fs_->disable();
        return Result::ok();
    }

    // One analysis result. A poll that yields no result is normal (no frame in
    // the window / transient) and reported as Timeout, never a failure - the
    // service treats timeouts as "no activity", not an error.
    Result poll(detection::DetectionResult& out, int timeout_ms) override {
        std::lock_guard<std::mutex> lk(m_);
        if (!recv_) return Result::busy();
        if (IMP_IVS_PollingResult(IVS_CHN, timeout_ms) != 0) return Result::timeout();

        IMP_IVS_MoveOutput* res = nullptr;
        if (IMP_IVS_GetResult(IVS_CHN, (void**)&res) != 0 || !res) return Result::timeout();
        int moved = 0;
        for (int i = 0; i < roi_cnt_; ++i) if (res->retRoi[i]) ++moved;
        IMP_IVS_ReleaseResult(IVS_CHN, res);

        out.motion = moved > 0;
        out.motion_level = roi_cnt_ > 0 ? (moved * 100 / roi_cnt_) : 0;
        if (out.motion) {
            detection::Detection d;
            d.label = "motion";
            d.confidence = out.motion_level;   // coarse: share of cells that moved
            out.detections.push_back(d);
        }
        return Result::ok();
    }

private:
    std::mutex m_;
    int  fs_chn_;
    // Declaration order = construction order; destruction is reverse, which
    // tears the IVS pipeline down in the correct order (channel, interface,
    // bind, framesource, group) and frees the move param last, after the
    // interface that may reference it.
    std::unique_ptr<IMP_IVS_MoveParam>        mp_;
    std::unique_ptr<IvsGroup>                 group_;
    std::unique_ptr<imp::FrameSourceChannel>  fs_;
    std::unique_ptr<imp::Binding>             bind_;
    std::unique_ptr<IvsMoveIface>             iface_;
    std::unique_ptr<IvsChannel>               chan_;
    int  roi_cnt_ = 0;
    bool recv_ = false;
};

// Fill a full-frame ROI grid; returns the number of cells written.
int build_grid(IMP_IVS_MoveParam& p, int w, int h, int sense) {
    const int cellw = w / GRID_COLS, cellh = h / GRID_ROWS;
    int idx = 0;
    for (int r = 0; r < GRID_ROWS; ++r) {
        for (int c = 0; c < GRID_COLS; ++c) {
            IMPRect& roi = p.roiRect[idx];
            roi.p0.x = c * cellw;
            roi.p0.y = r * cellh;
            roi.p1.x = (c == GRID_COLS - 1) ? (w - 1) : ((c + 1) * cellw - 1);   // last cell absorbs the remainder
            roi.p1.y = (r == GRID_ROWS - 1) ? (h - 1) : ((r + 1) * cellh - 1);
            p.sense[idx] = sense;
            ++idx;
        }
    }
    p.roiRectCnt = idx;
    return idx;
}

} // namespace

std::unique_ptr<IDetector> create_motion_detector(int chn, const DetectorParams& p, int native_w, int native_h) {
    // Analysis geometry: a downscaled channel is plenty for motion. Respect an
    // explicit request, otherwise ~640-wide preserving the sensor aspect.
    int aw = p.source_width  > 0 ? p.source_width  : 640;
    int ah = p.source_height > 0 ? p.source_height : (native_w > 0 ? (native_h * 640 / native_w) : 360);
    if (native_w > 0 && aw > native_w) aw = native_w;
    if (native_h > 0 && ah > native_h) ah = native_h;
    aw &= ~1; ah &= ~1;                                   // keep even dimensions for NV12
    if (aw < GRID_COLS * 2 || ah < GRID_ROWS * 2) { LOGE(MOD, "analysis geometry too small (%dx%d)", aw, ah); return nullptr; }
    const int fps = p.inference_fps > 0 ? p.inference_fps : 5;

    // Analysis FrameSource channel (own channel, scaled from the sensor).
    IMPFSChnAttr fa; memset(&fa, 0, sizeof fa);
    fa.pixFmt        = PIX_FMT_NV12;
    fa.outFrmRateNum = fps;                              // inference cadence is independent of the video fps
    fa.outFrmRateDen = 1;
    fa.nrVBs         = 2;
    fa.type          = FS_PHY_CHANNEL;
    fa.picWidth      = aw;
    fa.picHeight     = ah;
    if (native_w > 0 && native_h > 0 && (aw != native_w || ah != native_h)) {
        fa.scaler.enable    = 1;
        fa.scaler.outwidth  = aw;
        fa.scaler.outheight = ah;
    }
    auto fs = std::make_unique<imp::FrameSourceChannel>(chn, fa);
    if (!fs->ok()) { LOGE(MOD, "ivs framesource chn%d failed (%d)", chn, fs->rc()); return nullptr; }

    auto group = std::make_unique<IvsGroup>(IVS_GRP);
    if (!group->ok()) return nullptr;

    // Bind the analysis FrameSource channel to the IVS group.
    IMPCell src = { DEV_ID_FS,  chn,     0 };
    IMPCell dst = { DEV_ID_IVS, IVS_GRP, 0 };
    auto bind = std::make_unique<imp::Binding>(src, dst);
    if (!bind->ok()) { LOGE(MOD, "bind FS%d->IVS%d failed (%d)", chn, IVS_GRP, bind->rc()); return nullptr; }

    // Move algorithm parameters. The detector owns this struct so it outlives
    // the interface, in case the SDK keeps a pointer to it rather than copying.
    auto mp = std::make_unique<IMP_IVS_MoveParam>();
    memset(mp.get(), 0, sizeof(IMP_IVS_MoveParam));
    mp->skipFrameCnt = 0;
    mp->frameInfo.width  = aw;
    mp->frameInfo.height = ah;
    const int sense = 2;                                  // mid of the 0..4 range for a standard camera
    const int roi_cnt = build_grid(*mp, aw, ah, sense);

    auto iface = std::make_unique<IvsMoveIface>(mp.get());
    if (!iface->ok()) return nullptr;
    auto chan = std::make_unique<IvsChannel>(IVS_GRP, IVS_CHN, iface->get());
    if (!chan->ok()) return nullptr;

    LOGI(MOD, "motion pipeline built: %dx%d@%d, %d cells (scaler=%d)", aw, ah, fps, roi_cnt, fa.scaler.enable);
    return std::unique_ptr<IDetector>(new IngenicMotionDetector(
        chn, std::move(mp), std::move(group), std::move(fs), std::move(bind), std::move(iface), std::move(chan), roi_cnt));
}

}} // namespace machino::ingenic
