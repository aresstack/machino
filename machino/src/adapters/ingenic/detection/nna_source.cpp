#include "adapters/ingenic/detection/nna_source.hpp"
#include "adapters/ingenic/imp_sessions.hpp"
#include "core/log.hpp"

#include <cstring>
#include <mutex>

#include <imp/imp_common.h>
#include <imp/imp_framesource.h>

namespace machino { namespace ingenic {

static const char* MOD = "ING_NNA";

namespace {

class IngenicNnaSource final : public IAnalysisSource {
public:
    IngenicNnaSource(std::unique_ptr<imp::FrameSourceChannel> fs, int aw, int ah)
        : fs_(std::move(fs)), aw_(aw), ah_(ah) {}

    ~IngenicNnaSource() override { stop(); }

    Result start() override {
        std::lock_guard<std::mutex> lk(m_);
        if (running_) return Result::ok();
        // Tiefe 1: die NNA holt gepacet ab; ein tieferer Puffer hiesse nur,
        // aeltere Bilder zu analysieren. GetFrame liefert dann das juengste.
        int rc = IMP_FrameSource_SetFrameDepth(fs_->chn(), 1);
        if (rc != 0) { LOGE(MOD, "SetFrameDepth(%d,1) failed (%d)", fs_->chn(), rc); return Result::error(rc); }
        Result er = fs_->enable();
        if (!er) { IMP_FrameSource_SetFrameDepth(fs_->chn(), 0); return er; }
        running_ = true;
        return Result::ok();
    }

    Result stop() override {
        std::lock_guard<std::mutex> lk(m_);
        if (!running_) return Result::ok();
        release_locked();
        fs_->disable();
        IMP_FrameSource_SetFrameDepth(fs_->chn(), 0);
        running_ = false;
        return Result::ok();
    }

    Result get(AnalysisFrame& out, int timeout_ms) override {
        std::lock_guard<std::mutex> lk(m_);
        if (!running_) return Result::busy();
        release_locked();                          // strikte Paarung erzwingen
        IMPFrameInfo* f = nullptr;
        // GetFrame blockiert bis zum naechsten Bild; das SDK kennt hier kein
        // Timeout-Argument, aber der Kanal laeuft mit fps>=1 -- laenger als
        // eine Sekunde heisst "Quelle steht", und das soll ein Fehler sein,
        // kein ewiges Haengen des Poll-Threads. GetFrameTimeout gibt es in
        // dieser SDK-Fassung nicht; wir verlassen uns auf die Kanalrate.
        (void)timeout_ms;
        const int rc = IMP_FrameSource_GetFrame(fs_->chn(), &f);
        if (rc != 0 || !f) return Result::timeout();
        held_ = f;
        out.data   = reinterpret_cast<const uint8_t*>(f->virAddr);
        out.size   = f->size;
        out.width  = (int)f->width;
        out.height = (int)f->height;
        out.stride = (int)f->width;               // NV12 aus dem Scaler: dicht gepackt
        out.pts_us = (int64_t)f->timeStamp;
        return Result::ok();
    }

    void release() override {
        std::lock_guard<std::mutex> lk(m_);
        release_locked();
    }

    int width()  const override { return aw_; }
    int height() const override { return ah_; }

private:
    void release_locked() {
        if (held_) { IMP_FrameSource_ReleaseFrame(fs_->chn(), held_); held_ = nullptr; }
    }

    std::mutex m_;
    std::unique_ptr<imp::FrameSourceChannel> fs_;
    IMPFrameInfo* held_ = nullptr;
    int aw_, ah_;
    bool running_ = false;
};

} // namespace

std::unique_ptr<IAnalysisSource> create_nna_source(int chn, int aw, int ah, int fps,
                                                   int native_w, int native_h) {
    IMPFSChnAttr fa; memset(&fa, 0, sizeof fa);
    fa.pixFmt        = PIX_FMT_NV12;
    fa.outFrmRateNum = fps > 0 ? fps : 5;
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
    if (!fs->ok()) { LOGE(MOD, "nna framesource chn%d failed (%d)", chn, fs->rc()); return nullptr; }
    LOGI(MOD, "analysis source: chn%d %dx%d@%d (scaler=%d)", chn, aw, ah, fa.outFrmRateNum, fa.scaler.enable);
    return std::unique_ptr<IAnalysisSource>(new IngenicNnaSource(std::move(fs), aw, ah));
}

}} // namespace machino::ingenic
