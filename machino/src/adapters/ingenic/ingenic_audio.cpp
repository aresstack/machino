#include "adapters/ingenic/ingenic_audio.hpp"
#include "core/log.hpp"
#include <cstring>
#include <string>
#include <imp/imp_audio.h>

namespace machino { namespace ingenic {

static const char* MOD = "ING_AUDIO";

// Device/channel 0 both ways: the only pair the inner codec has.
static const int kDev = 0, kChn = 0;
// Ring of cached frames in the SDK and the user frame depth of the AI
// channel. The depth is REQUIRED: without it the T-series AI delivers no
// frames at all (PollingFrame stays empty -> silent audio). Low depth = low
// latency; the values are the ones the C prototype ran in the field.
static const int kFrmNum = 6;
static const int kUsrFrmDepth = 4;

bool audio_rate_supported(int hz) { return hz == 8000 || hz == 16000; }

static bool io_attr(const AudioParams& p, IMPAudioIOAttr& a) {
    if (!audio_rate_supported(p.sample_rate)) return false;
    const int n = p.samples_per_frame();
    if (n <= 0) return false;
    memset(&a, 0, sizeof a);
    a.samplerate = (IMPAudioSampleRate)p.sample_rate;
    a.bitwidth   = AUDIO_BIT_WIDTH_16;
    a.soundmode  = AUDIO_SOUND_MODE_MONO;
    a.frmNum     = kFrmNum;
    a.numPerFrm  = n;
    a.chnCnt     = 1;
    return true;
}

static int clamp(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

// ---------------------------------------------------------------- input ---

std::unique_ptr<IngenicAudioIn> IngenicAudioIn::create(const AudioParams& p) {
    IMPAudioIOAttr a;
    if (!io_attr(p, a)) { LOGE(MOD, "in: %d Hz / %d ms is not a rate this codec runs (8000|16000)", p.sample_rate, p.frame_ms); return nullptr; }
    auto self = std::unique_ptr<IngenicAudioIn>(new IngenicAudioIn());
    int r = IMP_AI_SetPubAttr(kDev, &a);
    if (r != 0) { LOGE(MOD, "IMP_AI_SetPubAttr(%d Hz, %d/frame) failed (%d)", p.sample_rate, a.numPerFrm, r); return nullptr; }
    r = IMP_AI_Enable(kDev);
    if (r != 0) { LOGE(MOD, "IMP_AI_Enable failed (%d)", r); return nullptr; }
    self->dev_enabled_ = true;
    IMPAudioIChnParam cp; memset(&cp, 0, sizeof cp);
    cp.usrFrmDepth = kUsrFrmDepth;
    r = IMP_AI_SetChnParam(kDev, kChn, &cp);
    if (r != 0) { LOGE(MOD, "IMP_AI_SetChnParam failed (%d)", r); return nullptr; }
    r = IMP_AI_EnableChn(kDev, kChn);
    if (r != 0) { LOGE(MOD, "IMP_AI_EnableChn failed (%d)", r); return nullptr; }
    self->chn_enabled_ = true;
    self->set_volume(p.volume);
    if (p.gain >= 0) self->set_gain(p.gain);
    LOGI(MOD, "microphone up: %d Hz mono, %d samples/frame, volume %d, gain %s",
         p.sample_rate, a.numPerFrm, p.volume, p.gain >= 0 ? std::to_string(p.gain).c_str() : "driver default");
    return self;
}

IngenicAudioIn::~IngenicAudioIn() {
    std::lock_guard<std::mutex> lk(m_);
    if (chn_enabled_) IMP_AI_DisableChn(kDev, kChn);
    if (dev_enabled_) IMP_AI_Disable(kDev);
    if (dev_enabled_) LOGI(MOD, "microphone down");
}

Result IngenicAudioIn::read(std::vector<int16_t>& pcm, int64_t& pts_us, int timeout_ms) {
    // PollingFrame blocks outside the lock, so a volume change never waits for
    // a poll timeout; GetFrame is then non-blocking.
    int r = IMP_AI_PollingFrame(kDev, kChn, (unsigned)(timeout_ms > 0 ? timeout_ms : 0));
    if (r != 0) return Result::timeout();
    std::lock_guard<std::mutex> lk(m_);
    IMPAudioFrame f; memset(&f, 0, sizeof f);
    r = IMP_AI_GetFrame(kDev, kChn, &f, NOBLOCK);
    if (r != 0) return Result::error(r);
    const size_t n = f.len > 0 ? (size_t)f.len / sizeof(int16_t) : 0;
    pcm.resize(n);
    if (n) memcpy(pcm.data(), f.virAddr, n * sizeof(int16_t));
    pts_us = f.timeStamp;
    IMP_AI_ReleaseFrame(kDev, kChn, &f);
    return Result::ok();
}

Result IngenicAudioIn::set_volume(int vol) {
    std::lock_guard<std::mutex> lk(m_);
    int r = IMP_AI_SetVol(kDev, kChn, clamp(vol, -30, 120));
    return r == 0 ? Result::ok() : Result::error(r);
}

Result IngenicAudioIn::set_gain(int gain) {
    std::lock_guard<std::mutex> lk(m_);
    int r = IMP_AI_SetGain(kDev, kChn, clamp(gain, 0, 31));
    return r == 0 ? Result::ok() : Result::error(r);
}

// --------------------------------------------------------------- output ---

std::unique_ptr<IngenicAudioOut> IngenicAudioOut::create(const AudioParams& p) {
    IMPAudioIOAttr a;
    if (!io_attr(p, a)) { LOGE(MOD, "out: %d Hz / %d ms is not a rate this codec runs (8000|16000)", p.sample_rate, p.frame_ms); return nullptr; }
    auto self = std::unique_ptr<IngenicAudioOut>(new IngenicAudioOut());
    int r = IMP_AO_SetPubAttr(kDev, &a);
    if (r != 0) { LOGE(MOD, "IMP_AO_SetPubAttr(%d Hz) failed (%d)", p.sample_rate, r); return nullptr; }
    r = IMP_AO_Enable(kDev);
    if (r != 0) { LOGE(MOD, "IMP_AO_Enable failed (%d)", r); return nullptr; }
    self->dev_enabled_ = true;
    r = IMP_AO_EnableChn(kDev, kChn);
    if (r != 0) { LOGE(MOD, "IMP_AO_EnableChn failed (%d)", r); return nullptr; }
    self->chn_enabled_ = true;
    self->rate_ = p.sample_rate;
    self->period_ = a.numPerFrm;
    self->set_volume(p.volume);
    LOGI(MOD, "speaker up: %d Hz mono, volume %d", p.sample_rate, p.volume);
    return self;
}

IngenicAudioOut::~IngenicAudioOut() {
    if (chn_enabled_) { IMP_AO_ClearChnBuf(kDev, kChn); IMP_AO_DisableChn(kDev, kChn); }
    if (dev_enabled_) { IMP_AO_Disable(kDev); LOGI(MOD, "speaker down"); }
}

Result IngenicAudioOut::write(const int16_t* pcm, size_t samples) {
    // SendFrame rejects a frame larger than the configured period, so feed it
    // period by period. BLOCK: the device queue is the playback clock.
    for (size_t off = 0; off < samples; ) {
        size_t chunk = samples - off;
        if (chunk > (size_t)period_) chunk = (size_t)period_;
        IMPAudioFrame f; memset(&f, 0, sizeof f);
        f.bitwidth  = AUDIO_BIT_WIDTH_16;
        f.soundmode = AUDIO_SOUND_MODE_MONO;
        f.virAddr   = (uint32_t*)(pcm + off);          // AO only reads it
        f.len       = (int)(chunk * sizeof(int16_t));
        int r = IMP_AO_SendFrame(kDev, kChn, &f, BLOCK);
        if (r != 0) return Result::error(r);
        off += chunk;
    }
    return Result::ok();
}

Result IngenicAudioOut::drain(int timeout_ms) {
    (void)timeout_ms;   // FlushChnBuf has no timeout: it returns when the cache has played out
    // SendFrame(BLOCK) only waits for ring space; the AO keeps its own cache
    // on top, so without this the last ~0.7 s of a clip is cut off.
    int r = IMP_AO_FlushChnBuf(kDev, kChn);
    return r == 0 ? Result::ok() : Result::error(r);
}

Result IngenicAudioOut::set_volume(int vol) {
    int r = IMP_AO_SetVol(kDev, kChn, clamp(vol, -30, 120));
    return r == 0 ? Result::ok() : Result::error(r);
}

}} // namespace machino::ingenic
