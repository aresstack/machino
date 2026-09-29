#include "core/audio/audio_encoder.hpp"
#include "core/log.hpp"

#ifdef MACHINO_CODECS
#include <faac.h>
#include <opus/opus.h>
#endif

namespace machino { namespace audio {

std::vector<uint8_t> aac_asc(int sample_rate, int channels) {
    static const int rates[13] = {96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350};
    int idx = 8;                                           // 16 kHz: never reached for the rates the codec runs
    for (int i = 0; i < 13; ++i) if (rates[i] == sample_rate) { idx = i; break; }
    const int obj = 2;                                     // AAC-LC
    return {(uint8_t)((obj << 3) | (idx >> 1)), (uint8_t)(((idx & 1) << 7) | (channels << 3))};
}

#ifdef MACHINO_CODECS

static const char* MOD = "AUDENC";

bool codecs_built() { return true; }

namespace {

// Re-blocks arbitrary input into fixed-size codec frames.
struct Reblock {
    std::vector<int16_t> buf;
    size_t frame = 0;
    template <class F> void feed(const int16_t* p, size_t n, F each) {
        buf.insert(buf.end(), p, p + n);
        size_t at = 0;
        while (buf.size() - at >= frame) { each(buf.data() + at); at += frame; }
        buf.erase(buf.begin(), buf.begin() + (long)at);
    }
};

class Aac final : public AudioEncoder {
public:
    static std::unique_ptr<Aac> create(int rate, int bitrate) {
        faac_params p;
        faac_params_init(&p);
        p.sample_rate = (uint32_t)rate;
        p.num_channels = 1;
        p.object_type = FAAC_OBJ_LOW;
        p.mpeg_version = FAAC_MPEG4;
        p.output_format = FAAC_STREAM_RAW;         // raw frames: the ASC travels in the container
        p.input_format = FAAC_INPUT_16BIT;
        p.bit_rate = (uint32_t)bitrate;            // per channel; mono
        std::unique_ptr<Aac> a(new Aac(rate));
        const faac_status st = faac_encoder_open(&p, &a->enc_);
        if (st != FAAC_OK || !a->enc_) { LOGW(MOD, "faac_encoder_open(%d Hz): %s", rate, faac_strerror(st)); return nullptr; }
        faac_encoder_info fi; fi.struct_size = sizeof fi;
        if (faac_encoder_get_info(a->enc_, &fi) != FAAC_OK) return nullptr;
        a->rb_.frame = fi.frame_samples;           // mono: samples == frame
        a->max_out_ = fi.max_output_bytes;
        a->out_.resize(a->max_out_);
        return a;
    }
    ~Aac() override { if (enc_) faac_encoder_close(&enc_); }
    const char* codec() const override { return "mp4a.40.2"; }
    int sample_rate() const override { return rate_; }
    uint32_t timescale() const override { return (uint32_t)rate_; }
    std::vector<uint8_t> config() const override {
        const uint8_t* b = nullptr; uint32_t n = 0;
        if (faac_encoder_asc(enc_, &b, &n) == FAAC_OK && b && n) return std::vector<uint8_t>(b, b + n);
        return aac_asc(rate_, 1);
    }
    bool encode(const int16_t* pcm, size_t n, std::vector<EncodedFrame>& out) override {
        bool ok = true;
        rb_.feed(pcm, n, [&](const int16_t* f) {
            uint32_t got = 0;
            const faac_status st = faac_encoder_encode(enc_, f, (uint32_t)rb_.frame, out_.data(), (uint32_t)out_.size(), &got);
            if (st != FAAC_OK) { ok = false; return; }
            // The encoder's first frames carry no output (its delay line);
            // an empty result is not an error.
            if (got == 0) return;
            EncodedFrame e;
            e.data.assign(out_.begin(), out_.begin() + (long)got);
            e.duration = (uint32_t)rb_.frame;
            out.push_back(std::move(e));
        });
        return ok;
    }
private:
    explicit Aac(int rate) : rate_(rate) {}
    int rate_;
    faac_encoder* enc_ = nullptr;
    Reblock rb_;
    size_t max_out_ = 0;
    std::vector<uint8_t> out_;
};

class Opus final : public AudioEncoder {
public:
    static std::unique_ptr<Opus> create(int rate, int bitrate) {
        int err = 0;
        OpusEncoder* e = opus_encoder_create(rate, 1, OPUS_APPLICATION_VOIP, &err);
        if (!e || err != OPUS_OK) { LOGW(MOD, "opus_encoder_create(%d Hz): %s", rate, opus_strerror(err)); return nullptr; }
        opus_encoder_ctl(e, OPUS_SET_BITRATE(bitrate));
        opus_encoder_ctl(e, OPUS_SET_COMPLEXITY(3));        // a camera CPU, not a studio
        opus_int32 lookahead = 0;
        opus_encoder_ctl(e, OPUS_GET_LOOKAHEAD(&lookahead));
        std::unique_ptr<Opus> o(new Opus(rate, e));
        o->pre_skip_ = (uint16_t)(lookahead * 48000 / rate);
        o->rb_.frame = (size_t)rate / 50;                   // 20 ms
        return o;
    }
    ~Opus() override { opus_encoder_destroy(enc_); }
    const char* codec() const override { return "opus"; }
    int sample_rate() const override { return rate_; }
    uint32_t timescale() const override { return 48000; }
    uint16_t pre_skip() const override { return pre_skip_; }
    bool encode(const int16_t* pcm, size_t n, std::vector<EncodedFrame>& out) override {
        bool ok = true;
        rb_.feed(pcm, n, [&](const int16_t* f) {
            uint8_t pkt[1500];
            const opus_int32 got = opus_encode(enc_, f, (int)rb_.frame, pkt, sizeof pkt);
            if (got < 0) { ok = false; return; }
            EncodedFrame e;
            e.data.assign(pkt, pkt + got);
            e.duration = 960;                               // 20 ms at 48 kHz
            out.push_back(std::move(e));
        });
        return ok;
    }
private:
    Opus(int rate, OpusEncoder* e) : rate_(rate), enc_(e) {}
    int rate_;
    OpusEncoder* enc_;
    uint16_t pre_skip_ = 0;
    Reblock rb_;
};

} // namespace

std::unique_ptr<AudioEncoder> make_aac_encoder(int sample_rate, int bitrate_bps) { return Aac::create(sample_rate, bitrate_bps); }
std::unique_ptr<AudioEncoder> make_opus_encoder(int sample_rate, int bitrate_bps) { return Opus::create(sample_rate, bitrate_bps); }

#else  // !MACHINO_CODECS

bool codecs_built() { return false; }
std::unique_ptr<AudioEncoder> make_aac_encoder(int, int) { return nullptr; }
std::unique_ptr<AudioEncoder> make_opus_encoder(int, int) { return nullptr; }

#endif

std::unique_ptr<AudioEncoder> make_encoder(const std::string& codec, int sample_rate) {
    if (codec == "opus") return make_opus_encoder(sample_rate);
    if (codec == "mp4a.40.2" || codec == "aac") return make_aac_encoder(sample_rate);
    return nullptr;
}

}} // namespace machino::audio
