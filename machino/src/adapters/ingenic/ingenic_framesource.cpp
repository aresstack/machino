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

}} // namespace machino::ingenic
