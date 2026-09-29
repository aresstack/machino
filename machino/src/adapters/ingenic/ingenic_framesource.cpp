#include "adapters/ingenic/ingenic_framesource.hpp"
#include "core/log.hpp"
#include <cstring>

namespace machino { namespace ingenic {

static const char* MOD = "ING_FS";

std::unique_ptr<IngenicFrameSource> IngenicFrameSource::create(int chn, const EffectiveStream& sc) {
    IMPFSChnAttr a; memset(&a, 0, sizeof a);
    a.pixFmt        = PIX_FMT_NV12;
    a.outFrmRateNum = sc.fps;
    a.outFrmRateDen = 1;
    a.nrVBs         = sc.buffers > 0 ? sc.buffers : 2;
    a.type          = FS_PHY_CHANNEL;
    a.picWidth      = sc.width;
    a.picHeight     = sc.height;
    if (sc.width != sc.native_width || sc.height != sc.native_height) {
        a.scaler.enable    = 1;
        a.scaler.outwidth  = sc.width;
        a.scaler.outheight = sc.height;
    }
    auto c = std::make_unique<imp::FrameSourceChannel>(chn, a);
    if (!c->ok()) return nullptr;
    LOGI(MOD, "chn%d %dx%d@%d nv12 vbs=%d scaler=%d", chn, sc.width, sc.height, sc.fps, a.nrVBs, a.scaler.enable);
    return std::unique_ptr<IngenicFrameSource>(new IngenicFrameSource(std::move(c)));
}

// IMP_FrameSource_SnapFrame copies the channel's current frame into our
// buffer: no SetFrameDepth (which would take frames away from the bound
// encoder), valid only while the channel is enabled.
Result IngenicFrameSource::snap_nv12(std::vector<uint8_t>& out, int w, int h) {
    if (w <= 0 || h <= 0) return Result::unsupported();
    out.resize((size_t)w * (size_t)h * 3 / 2);
    IMPFrameInfo info; memset(&info, 0, sizeof info);
    const int rc = IMP_FrameSource_SnapFrame(chan_->chn(), PIX_FMT_NV12, w, h, out.data(), &info);
    if (rc != 0) { LOGW(MOD, "SnapFrame(chn%d %dx%d) failed (%d)", chan_->chn(), w, h, rc); out.clear(); return Result::error(rc); }
    return Result::ok();
}

}} // namespace machino::ingenic
