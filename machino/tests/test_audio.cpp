// Audio: G.711 against reference values, the volume curves, the audio config
// keys, and the AudioService lifecycle ("no listener, no microphone") against
// a fake codec input.
#include "app/audio_test.hpp"
#include "app/compat/majestic_migrate.hpp"
#include "app/compat/majestic_webui.hpp"
#include "app/http/audio_stream.hpp"
#include "app/http/fmp4.hpp"
#include "app/http/ogg.hpp"
#include "core/audio/audio_encoder.hpp"
#include "app/rtsp/rtp_audio.hpp"
#include "core/audio/audio_service.hpp"
#include "core/audio/g711.hpp"
#include "core/json.hpp"
#include "core/config.hpp"

#include <atomic>
#include <cmath>
#ifdef MACHINO_CODECS
#include <opus/opus.h>
#endif
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

using namespace machino;

extern int g_fail_ext, g_pass_ext;
#define ACHECK(cond) do { if (cond) { ++g_pass_ext; } else { ++g_fail_ext; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

namespace {

struct FakeMicCounters {
    std::atomic<int> opens{0}, closes{0}, volume{-999}, gain{-999};
    std::atomic<int> last_rate{0};
    std::atomic<bool> refuse{false};
};

class FakeMic final : public IAudioIn {
public:
    FakeMic(FakeMicCounters& c, const AudioParams& p) : c_(c), n_(p.samples_per_frame()) { c_.opens++; c_.last_rate = p.sample_rate; c_.volume = p.volume; }
    ~FakeMic() override { c_.closes++; }
    Result read(std::vector<int16_t>& pcm, int64_t& pts_us, int timeout_ms) override {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        pcm.assign((size_t)n_, 0);
        for (int i = 0; i < n_; ++i) pcm[(size_t)i] = (int16_t)(seq_ * 100 + i);
        pts_us = (int64_t)seq_++ * 40000;
        return Result::ok();
    }
    Result set_volume(int v) override { c_.volume = v; return Result::ok(); }
    Result set_gain(int g) override { c_.gain = g; return Result::ok(); }
private:
    FakeMicCounters& c_;
    int n_;
    int seq_ = 0;
};

audio::AudioService::InFactory fake_factory(FakeMicCounters& c) {
    return [&c](const AudioParams& p) -> std::unique_ptr<IAudioIn> {
        if (c.refuse) return nullptr;
        return std::unique_ptr<IAudioIn>(new FakeMic(c, p));
    };
}

template <class F> bool eventually(F f, int ms = 2000) {
    for (int i = 0; i < ms / 5; ++i) { if (f()) return true; std::this_thread::sleep_for(std::chrono::milliseconds(5)); }
    return f();
}

void test_g711_reference() {
    // Silence and the well-known code points (ITU-T G.711 / Sun g711.c).
    ACHECK(audio::ulaw_encode((int16_t)0) == 0xFF);
    ACHECK(audio::alaw_encode((int16_t)0) == 0xD5);
    ACHECK(audio::ulaw_encode((int16_t)32767) == 0x80);
    ACHECK(audio::ulaw_encode((int16_t)-32768) == 0x00);
    ACHECK(audio::alaw_encode((int16_t)32767) == 0xAA);
    ACHECK(audio::alaw_encode((int16_t)-32768) == 0x2A);
    ACHECK(audio::ulaw_decode(0xFF) == 0);
    ACHECK(audio::alaw_decode(0xD5) == 8);           // A-law has no exact zero: the smallest step
    ACHECK(audio::ulaw_decode(0x80) == 32124);
    ACHECK(audio::alaw_decode(0xAA) == 32256);
    // Round trip stays within the quantisation step of its segment and keeps the sign.
    bool ok_u = true, ok_a = true;
    for (int s = -32000; s <= 32000; s += 37) {
        const int du = audio::ulaw_decode(audio::ulaw_encode((int16_t)s));
        const int da = audio::alaw_decode(audio::alaw_encode((int16_t)s));
        const int tol = std::abs(s) / 16 + 16;
        if (std::abs(du - s) > tol || (s > 100 && du <= 0) || (s < -100 && du >= 0)) ok_u = false;
        if (std::abs(da - s) > tol || (s > 100 && da <= 0) || (s < -100 && da >= 0)) ok_a = false;
    }
    ACHECK(ok_u);
    ACHECK(ok_a);
    // The buffer variants agree with the per-sample ones.
    const int16_t in[4] = {0, 1000, -1000, 30000};
    uint8_t u[4], a[4];
    audio::ulaw_encode(in, 4, u);
    audio::alaw_encode(in, 4, a);
    bool same = true;
    for (int i = 0; i < 4; ++i) same = same && u[i] == audio::ulaw_encode(in[i]) && a[i] == audio::alaw_encode(in[i]);
    ACHECK(same);
}

void test_volume_curves() {
    ACHECK(audio::input_volume_to_vendor(0) == -30);           // mute
    ACHECK(audio::input_volume_to_vendor(30) == 30);           // majestic default
    ACHECK(audio::input_volume_to_vendor(80) == 80);           // stock InputVol 80
    ACHECK(audio::input_volume_to_vendor(100) == 100);
    ACHECK(audio::input_volume_to_vendor(35) == 35);           // interpolated between 30 and 40
    ACHECK(audio::input_volume_to_vendor(250) == 100);         // clamped
    ACHECK(audio::output_volume_to_vendor(90) == 85);          // stock OutputVol 90
    ACHECK(audio::output_volume_to_vendor(-5) == -30);
    bool monotonic = true;
    for (int p = 1; p <= 100; ++p) monotonic = monotonic && audio::input_volume_to_vendor(p) >= audio::input_volume_to_vendor(p - 1)
                                                         && audio::output_volume_to_vendor(p) >= audio::output_volume_to_vendor(p - 1);
    ACHECK(monotonic);
}

void test_audio_config() {
    AppConfig c; std::string err;
    ACHECK(!c.audio.enabled && c.audio.srate == 8000 && c.audio.volume == 30 && !c.audio.output_enabled);
    ACHECK(parse_config_text("audio.enabled = true\naudio.srate = 16000\naudio.volume = 70\naudio.gain = 20\n"
                             "audio.output_enabled = true\naudio.output_volume = 55\naudio.grace_ms = 500\n", c, err));
    ACHECK(c.audio.enabled && c.audio.srate == 16000 && c.audio.volume == 70 && c.audio.gain == 20);
    ACHECK(c.audio.output_enabled && c.audio.output_volume == 55 && c.audio.grace_ms == 500);
    // A rate the codec cannot run is refused, not stored.
    ACHECK(parse_config_text("audio.srate = 48000\naudio.volume = 101\n", c, err));
    ACHECK(c.audio.srate == 16000 && c.audio.volume == 70);
}

void test_service_refusals() {
    AudioConfig cfg;               // enabled = false
    FakeMicCounters c;
    audio::AudioService off(cfg, fake_factory(c));
    std::string why;
    ACHECK(!off.listen(why) && why.find("disabled") != std::string::npos);
    ACHECK(c.opens == 0);          // a disabled microphone is never opened

    cfg.enabled = true;
    audio::AudioService none(cfg, nullptr);
    why.clear();
    ACHECK(!none.available() && !none.listen(why) && why.find("no audio input") != std::string::npos);

    c.refuse = true;
    audio::AudioService refused(cfg, fake_factory(c));
    why.clear();
    ACHECK(!refused.listen(why) && why.find("refused") != std::string::npos);
    ACHECK(refused.stats().last_error == why && !refused.stats().capturing);
}

void test_service_lifecycle() {
    AudioConfig cfg; cfg.enabled = true; cfg.grace_ms = 60; cfg.volume = 80; cfg.srate = 16000;
    FakeMicCounters c;
    audio::AudioService svc(cfg, fake_factory(c));
    ACHECK(svc.available() && c.opens == 0 && !svc.stats().capturing);   // nothing open without a listener

    std::string why;
    auto a = svc.listen(why);
    ACHECK(a != nullptr && c.opens == 1 && c.last_rate == 16000);
    ACHECK(c.volume == audio::input_volume_to_vendor(80));               // volume applied at open, vendor scale
    AuPtr au; bool disc = false;
    ACHECK(a->pop(au, 1000, &disc) && au && au->key && au->data.size() == 640 * sizeof(int16_t));

    auto b = svc.listen(why);                                             // a second listener shares the device
    ACHECK(b != nullptr && c.opens == 1 && svc.stats().listeners == 2);
    ACHECK(b->pop(au, 1000));

    svc.set_volume(50);
    ACHECK(c.volume == audio::input_volume_to_vendor(50));               // live on the open device
    svc.set_gain(12);
    ACHECK(c.gain == 12);

    svc.unlisten(a);
    ACHECK(svc.stats().capturing);                                         // one listener left
    svc.unlisten(b);
    ACHECK(eventually([&] { return c.closes == 1; }));                     // closed after the grace
    ACHECK(!svc.stats().capturing);

    auto again = svc.listen(why);                                          // re-opens cleanly
    ACHECK(again != nullptr && eventually([&] { return c.opens == 2; }));
    ACHECK(again->pop(au, 1000));
    // A returning listener inside the grace keeps the device: no reopen.
    svc.unlisten(again);
    auto quick = svc.listen(why);
    ACHECK(quick != nullptr && c.opens == 2);
    svc.unlisten(quick);
    ACHECK(eventually([&] { return c.closes == 2; }));
    ACHECK(svc.stats().starts == 2 && svc.stats().frames > 0);
}

void test_service_disable_drops_listeners() {
    AudioConfig cfg; cfg.enabled = true; cfg.grace_ms = 10000;
    FakeMicCounters c;
    audio::AudioService svc(cfg, fake_factory(c));
    std::string why;
    auto s = svc.listen(why);
    ACHECK(s != nullptr);
    svc.set_enabled(false);
    ACHECK(s->closed());                                                    // dropped now, not at the next frame
    ACHECK(eventually([&] { return c.closes == 1; }));                      // and the device closes despite the long grace
    ACHECK(!svc.listen(why));
    svc.set_enabled(true);
    auto t = svc.listen(why);
    ACHECK(t != nullptr && eventually([&] { return c.opens == 2; }));
    svc.shutdown();
    ACHECK(t->closed() && c.closes == 2);
    ACHECK(!svc.listen(why) && why == "shutting down");
}

void test_http_audio_stream() {
    using namespace http;
    ACHECK(audio_format_for_path("/audio.pcm") == AudioFormat::Pcm);
    ACHECK(audio_format_for_path("/audio.alaw") == AudioFormat::Alaw);
    ACHECK(audio_format_for_path("/audio.g711a") == AudioFormat::Alaw);    // majestic's second name for the same stream
    ACHECK(audio_format_for_path("/audio.ulaw") == AudioFormat::Ulaw);
    ACHECK(audio_format_for_path("/audio.opus") == AudioFormat::Opus);
    ACHECK(audio_format_for_path("/audio.m4a") == AudioFormat::Aac);
    ACHECK(audio_format_for_path("/audio.pcm.x") == AudioFormat::None);

    ACHECK(audio_wire_rate(AudioFormat::Pcm, 16000) == 16000);
    ACHECK(audio_wire_rate(AudioFormat::Alaw, 16000) == 8000);             // G.711 is 8 kHz by definition
    const std::string hp = audio_stream_headers(AudioFormat::Pcm, 16000);
    ACHECK(hp.rfind("HTTP/1.1 200 OK\r\n", 0) == 0 && hp.find("rate=16000") != std::string::npos);
    ACHECK(hp.find("format=s16le") != std::string::npos && hp.find("audio/L16") == std::string::npos);
    ACHECK(hp.find("Connection: close") != std::string::npos && hp.find("Content-Length") == std::string::npos);
    ACHECK(hp.size() > 4 && hp.compare(hp.size() - 4, 4, "\r\n\r\n") == 0);
    const std::string ha = audio_stream_headers(AudioFormat::Alaw, 16000);
    ACHECK(ha.find("audio/PCMA") != std::string::npos && ha.find("X-Audio-Rate: 8000") != std::string::npos);
    ACHECK(audio_stream_headers(AudioFormat::Ulaw, 8000).find("audio/PCMU") != std::string::npos);

    // PCM passes the bytes through unchanged.
    const int16_t pcm[4] = {0, 1000, -1000, 32767};
    const uint8_t* b = reinterpret_cast<const uint8_t*>(pcm);
    std::string out;
    audio_encode(AudioFormat::Pcm, 8000, b, sizeof pcm, out);
    ACHECK(out.size() == sizeof pcm && memcmp(out.data(), pcm, sizeof pcm) == 0);
    // G.711 at 8 kHz: one byte per sample, the codec's own values.
    out.clear();
    audio_encode(AudioFormat::Ulaw, 8000, b, sizeof pcm, out);
    ACHECK(out.size() == 4 && (uint8_t)out[0] == 0xFF && (uint8_t)out[3] == audio::ulaw_encode((int16_t)32767));
    // G.711 from 16 kHz: every pair averaged, half the bytes. Appends.
    const int16_t wide[4] = {1000, 3000, -2000, -4000};
    audio_encode(AudioFormat::Alaw, 16000, reinterpret_cast<const uint8_t*>(wide), sizeof wide, out);
    ACHECK(out.size() == 6);
    ACHECK((uint8_t)out[4] == audio::alaw_encode((int16_t)2000) && (uint8_t)out[5] == audio::alaw_encode((int16_t)-3000));
    std::string none;
    audio_encode(AudioFormat::None, 8000, b, sizeof pcm, none);
    ACHECK(none.empty());
}

void test_audio_test_helpers() {
    const std::vector<uint8_t> h = app::wav_header(16000, 100);
    ACHECK(h.size() == 44 && memcmp(h.data(), "RIFF", 4) == 0 && memcmp(h.data() + 8, "WAVEfmt ", 8) == 0);
    ACHECK(h[24] == 0x80 && h[25] == 0x3E);                   // 16000 little-endian
    ACHECK(h[40] == 200 && h[4] == 236);                      // data bytes, RIFF size = 36 + data

    // A written header plus samples parses back to the same rate and samples.
    std::vector<uint8_t> file = h;
    std::vector<int16_t> src(100);
    for (int i = 0; i < 100; ++i) src[(size_t)i] = (int16_t)(i * 300 - 15000);
    const uint8_t* sb = reinterpret_cast<const uint8_t*>(src.data());
    file.insert(file.end(), sb, sb + src.size() * 2);
    int rate = 0; std::vector<int16_t> back; std::string err;
    ACHECK(app::wav_parse(file, rate, back, err) && rate == 16000 && back == src);
    // Refusals say what is wrong.
    std::vector<uint8_t> stereo = file; stereo[22] = 2;
    ACHECK(!app::wav_parse(stereo, rate, back, err) && err.find("mono") != std::string::npos);
    std::vector<uint8_t> junk(20, 0);
    ACHECK(!app::wav_parse(junk, rate, back, err) && err.find("RIFF") != std::string::npos);
    std::vector<uint8_t> cut(file.begin(), file.begin() + 60);
    ACHECK(!app::wav_parse(cut, rate, back, err) && err.find("truncated") != std::string::npos);

    // Level meter: silence is the floor, a -6 dBFS sine reads as such.
    double rms = 0, peak = 0;
    app::level_dbfs(std::vector<int16_t>(320, 0), rms, peak);
    ACHECK(rms == -120.0 && peak == -120.0);
    std::vector<int16_t> tone; double phase = 0;
    app::sine(tone, 8000, 8000, 1000, 0.5, phase);
    app::level_dbfs(tone, rms, peak);
    ACHECK(peak > -6.1 && peak < -5.9);                        // amplitude 0.5
    ACHECK(rms > -9.1 && rms < -8.9);                          // sine RMS = peak - 3 dB
    // The phase carries over, so chunks join without a click.
    std::vector<int16_t> a, bb; double ph = 0;
    app::sine(a, 3, 8000, 1000, 0.5, ph);
    app::sine(bb, 3, 8000, 1000, 0.5, ph);
    std::vector<int16_t> whole; double pw = 0;
    app::sine(whole, 6, 8000, 1000, 0.5, pw);
    ACHECK(a[0] == whole[0] && bb[0] == whole[3] && bb[2] == whole[5]);
}

void test_majestic_audio_mapping() {
    using namespace compat;
    // Schema: a microphone section only when the platform has one; no speaker switch.
    Json caps = Json::object();
    Json noaud = majestic_schema(caps);
    ACHECK(!noaud.get("properties")->get("audio"));
    Json au = Json::object(); au.set("input", Json::boolean(true)); au.set("output", Json::boolean(false));
    Json rates = Json::array(); rates.push(Json::integer(8000)); rates.push(Json::integer(16000)); au.set("sample_rates", rates);
    caps.set("audio", au);
    Json schema = majestic_schema(caps);
    const Json* sec = schema.get("properties")->get("audio");
    ACHECK(sec && sec->get("properties"));
    const Json* fields = sec ? sec->get("properties") : nullptr;
    ACHECK(fields && fields->get("enabled") && fields->get("volume") && fields->get("srate"));
    ACHECK(fields && !fields->get("outputEnabled") && !fields->get("outputVolume"));
    ACHECK(fields && fields->get("srate")->get("enum")->size() == 2);
    ACHECK(fields && fields->get("volume")->get("maximum")->as_int() == 100);
    // With an output the speaker switch and level appear too.
    au.set("output", Json::boolean(true));
    caps.set("audio", au);
    const Json with_out = majestic_schema(caps);             // kept alive: sf points into it
    const Json* sf = with_out.get("properties")->get("audio")->get("properties");
    ACHECK(sf && sf->get("outputEnabled") && sf->get("outputVolume") && sf->get("enabled"));

    // Config: both switches ALWAYS present (absent is not false); the speaker stays false.
    Json none = majestic_config(Json::object(), Json::object());
    ACHECK(none.get("audio") && !none.get("audio")->get("enabled")->as_bool() && !none.get("audio")->get("outputEnabled")->as_bool());
    Json native = Json::object();
    Json na = Json::object();
    na.set("enabled", Json::boolean(true)); na.set("srate", Json::integer(16000)); na.set("volume", Json::integer(55));
    na.set("output_enabled", Json::boolean(true));
    native.set("audio", na);
    Json mc = majestic_config(native, Json::object());
    const Json* ma = mc.get("audio");
    ACHECK(ma && ma->get("enabled")->as_bool() && ma->get("srate")->as_int() == 16000 && ma->get("volume")->as_int() == 55);
    ACHECK(ma && ma->get("outputEnabled")->as_bool());           // the speaker switch round-trips

    // Migration from majestic.yaml.
    MigrationResult r = migrate_majestic_yaml("audio:\n  enabled: true\n  volume: 45\n  srate: 48000\n  codec: opus\n  outputEnabled: true\n");
    auto disp = [&](const char* k) { for (const auto& e : r.entries) if (e.source_key == k) return (int)e.disp; return -1; };
    ACHECK(disp("audio.enabled") == (int)Disposition::Mapped);
    ACHECK(disp("audio.volume") == (int)Disposition::Mapped);
    ACHECK(disp("audio.srate") == (int)Disposition::Converted);  // 48000 -> the nearest rate the codec runs
    ACHECK(disp("audio.codec") == (int)Disposition::Unsupported);
    ACHECK(disp("audio.outputEnabled") == (int)Disposition::Mapped);
    const std::string conf = to_machino_conf(r);
    ACHECK(conf.find("audio.enabled = true") != std::string::npos && conf.find("audio.srate = 16000") != std::string::npos);
    AppConfig c; std::string err;
    ACHECK(parse_config_text(conf, c, err) && c.audio.enabled && c.audio.volume == 45 && c.audio.srate == 16000);
}

// The majestic URLs without a path in this build get a named 501, and the
// ones that ARE served natively never land on that list.
void test_majestic_unbuilt_urls() {
    using compat::majestic_unbuilt;
    for (const char* u : {"/video.mp4", "/hls", "/hls/index.m3u8", "/image.heif", "/image.yuv420"}) {
        const char* why = majestic_unbuilt(u);
        ACHECK(why != nullptr && std::string(why).size() > 20);
    }
    for (const char* u : {"/mjpeg", "/mjpeg.html", "/image.jpg", "/audio.pcm", "/audio.alaw", "/audio.ulaw",
                          "/audio.g711a", "/play_audio", "/night/on", "/metrics", "/api/v1/config.json", "/hlsx", "/"}) {
        ACHECK(majestic_unbuilt(u) == nullptr);
    }
}

struct FakeSpkCounters {
    std::atomic<int> opens{0}, closes{0}, drains{0}, volume{-999}, last_rate{0};
    std::atomic<size_t> samples{0};
    std::atomic<bool> refuse{false}, fail_write{false};
    std::atomic<int> write_delay_ms{0};
};

class FakeSpk final : public IAudioOut {
public:
    FakeSpk(FakeSpkCounters& c, const AudioParams& p) : c_(c), rate_(p.sample_rate) { c_.opens++; c_.last_rate = p.sample_rate; c_.volume = p.volume; }
    ~FakeSpk() override { c_.closes++; }
    Result write(const int16_t*, size_t n) override {
        if (c_.write_delay_ms) std::this_thread::sleep_for(std::chrono::milliseconds(c_.write_delay_ms.load()));
        if (c_.fail_write) return Result::error();
        c_.samples += n; return Result::ok();
    }
    Result drain(int) override { c_.drains++; return Result::ok(); }
    Result set_volume(int v) override { c_.volume = v; return Result::ok(); }
    int sample_rate() const override { return rate_; }
private:
    FakeSpkCounters& c_;
    int rate_;
};

audio::Speaker::OutFactory spk_factory(FakeSpkCounters& c) {
    return [&c](const AudioParams& p) -> std::unique_ptr<IAudioOut> {
        if (c.refuse) return nullptr;
        return std::unique_ptr<IAudioOut>(new FakeSpk(c, p));
    };
}

void test_speaker() {
    std::string why;
    // No output on this platform: said, not pretended.
    audio::Speaker none(nullptr, true, 50, 50);
    ACHECK(!none.available() && none.play(std::vector<int16_t>(80), 8000, why).status == Status::Unsupported);
    ACHECK(why.find("no audio output") != std::string::npos);

    FakeSpkCounters c;
    {
        audio::Speaker off(spk_factory(c), false, 50, 50);
        ACHECK(off.play(std::vector<int16_t>(80), 8000, why).status == Status::Busy && why.find("switched off") != std::string::npos);
        ACHECK(c.opens == 0);                                              // switched off: never opened
    }

    audio::Speaker spk(spk_factory(c), true, 90, 60);
    ACHECK(c.opens == 0 && !spk.stats().open);                             // no clip, no speaker
    ACHECK(spk.play(std::vector<int16_t>(800), 16000, why));
    ACHECK(spk.play(std::vector<int16_t>(400), 16000, why));
    ACHECK(spk.wait_idle(2000));
    ACHECK(c.opens == 1 && c.samples == 1200 && c.last_rate == 16000);    // two clips, one open
    ACHECK(c.volume == audio::output_volume_to_vendor(90));                // stock-calibrated output curve
    ACHECK(c.drains == 0 && spk.stats().clips == 2);                      // back to back: no flush between clips
    ACHECK(eventually([&] { return c.closes == 1 && !spk.stats().open; }));   // closed after the grace...
    ACHECK(c.drains == 1);                                                 // ...having played out first

    // A clip at another rate reopens at that rate.
    ACHECK(spk.play(std::vector<int16_t>(80), 8000, why) && spk.wait_idle(2000));
    ACHECK(c.opens == 2 && c.last_rate == 8000);
    spk.set_volume(20);
    ACHECK(eventually([&] { return c.closes == 2; }));

    // Refusals.
    ACHECK(spk.play(std::vector<int16_t>(80), 44100, why).status == Status::Unsupported && why.find("44100") != std::string::npos);
    ACHECK(!spk.play(std::vector<int16_t>(), 8000, why) && why.find("empty") != std::string::npos);
    const size_t cap = (size_t)audio::Speaker::kMaxQueueSeconds * 8000;
    c.write_delay_ms = 20;                                                 // keep the first clip busy
    ACHECK(spk.play(std::vector<int16_t>(8000), 8000, why));
    ACHECK(spk.play(std::vector<int16_t>(cap - 100), 8000, why));
    ACHECK(spk.play(std::vector<int16_t>(8000), 8000, why).status == Status::Busy && why.find("full") != std::string::npos);

    // Off cuts what plays and drops what waits.
    spk.set_enabled(false);
    ACHECK(spk.wait_idle(3000));
    ACHECK(spk.stats().dropped >= 1 && spk.stats().queued_ms == 0);
    c.write_delay_ms = 0;
    spk.set_enabled(true);

    // A device that refuses to open, or fails to write, is reported.
    ACHECK(eventually([&] { return !spk.stats().open; }));               // the cut clip's output has closed
    c.refuse = true;
    ACHECK(spk.play(std::vector<int16_t>(80), 8000, why) && spk.wait_idle(2000));
    ACHECK(spk.stats().last_error.find("refused") != std::string::npos);
    c.refuse = false; c.fail_write = true;
    ACHECK(spk.play(std::vector<int16_t>(80), 8000, why) && spk.wait_idle(2000));
    ACHECK(spk.stats().last_error.find("writing") != std::string::npos && !spk.stats().open);
    c.fail_write = false;
    spk.shutdown();
    ACHECK(spk.play(std::vector<int16_t>(80), 8000, why).status == Status::Busy);

    // Through the AudioService: the switch and the level are live.
    FakeSpkCounters sc;
    AudioConfig ac; ac.output_enabled = false; ac.output_volume = 40; ac.grace_ms = 30;
    audio::AudioService svc(ac, nullptr, spk_factory(sc));
    ACHECK(svc.output_available() && !svc.available());
    ACHECK(svc.play(std::vector<int16_t>(80), 8000, why).status == Status::Busy);
    svc.set_output_enabled(true);
    ACHECK(svc.play(std::vector<int16_t>(80), 8000, why) && svc.wait_speaker_idle(2000));
    ACHECK(sc.opens == 1 && sc.volume == audio::output_volume_to_vendor(40));
    ACHECK(svc.config().output_enabled && svc.speaker_stats().clips == 1);
}

void test_play_body() {
    std::vector<int16_t> pcm; int rate = 0; std::string err;
    // Raw: the camera's own rate, little-endian pairs; an odd byte is dropped.
    const std::string raw("\x01\x00\xff\x7f\x00\x80\x05", 7);
    ACHECK(http::play_body_to_pcm(raw, 16000, pcm, rate, err) && rate == 16000 && pcm.size() == 3);
    ACHECK(pcm[0] == 1 && pcm[1] == 32767 && pcm[2] == -32768);
    // WAV: its own rate.
    std::vector<uint8_t> w = app::wav_header(8000, 2);
    w.insert(w.end(), {0x10, 0x00, 0xf0, 0xff});
    ACHECK(http::play_body_to_pcm(std::string(w.begin(), w.end()), 16000, pcm, rate, err) && rate == 8000 && pcm.size() == 2 && pcm[1] == -16);
    // Refusals say why.
    ACHECK(!http::play_body_to_pcm(std::string(1, 'x'), 8000, pcm, rate, err) && err.find("no samples") != std::string::npos);
    std::vector<uint8_t> st = app::wav_header(8000, 2); st[22] = 2; st.insert(st.end(), 8, 0);
    ACHECK(!http::play_body_to_pcm(std::string(st.begin(), st.end()), 8000, pcm, rate, err) && err.find("mono") != std::string::npos);
    ACHECK(http::kMaxPlayBodyBytes / 2 <= (size_t)audio::Speaker::kMaxQueueSeconds * 8000);   // a maximal body fits the queue
}

void test_rtsp_backchannel_helpers() {
    const std::string sdp = rtsp::sdp_backchannel_section();
    ACHECK(sdp.rfind("m=audio 0 RTP/AVP 0 8\r\n", 0) == 0 && sdp.find("a=sendonly\r\n") != std::string::npos);
    ACHECK(sdp.find("a=control:trackID=2\r\n") != std::string::npos);
    ACHECK(rtsp::wants_backchannel("www.onvif.org/ver20/backchannel"));
    ACHECK(rtsp::wants_backchannel("WWW.ONVIF.ORG/ver20/Backchannel, other"));
    ACHECK(!rtsp::wants_backchannel("") && !rtsp::wants_backchannel("play.basic"));

    // Interleaved frames are taken off the front, requests left alone.
    std::string buf("$\x04\x00\x03" "abc" "$\x01\x00\x02" "zzOPTIONS rtsp://x RTSP/1.0\r\n\r\n", 4 + 3 + 4 + 2 + 34);
    int ch = -1; std::string data;
    ACHECK(rtsp::take_interleaved(buf, ch, data) == rtsp::Interleaved::Frame && ch == 4 && data == "abc");
    ACHECK(rtsp::take_interleaved(buf, ch, data) == rtsp::Interleaved::Frame && ch == 1 && data == "zz");
    ACHECK(rtsp::take_interleaved(buf, ch, data) == rtsp::Interleaved::NotOne && buf.rfind("OPTIONS", 0) == 0);
    std::string partial("$\x00\x00\x10" "abc", 7);
    ACHECK(rtsp::take_interleaved(partial, ch, data) == rtsp::Interleaved::Partial && partial.size() == 7);
    std::string head("$\x00", 2);
    ACHECK(rtsp::take_interleaved(head, ch, data) == rtsp::Interleaved::Partial);

    // RTP payload location, then G.711 to PCM.
    std::vector<uint8_t> rtp = {0x80, 0, 0, 1, 0, 0, 0, 0, 1, 2, 3, 4, 0xFF, 0x80, 0x00};
    uint8_t pt = 99; size_t off = 0, len = 0;
    ACHECK(rtsp::rtp_payload(rtp.data(), rtp.size(), pt, off, len) && pt == 0 && off == 12 && len == 3);
    std::vector<int16_t> pcm;
    ACHECK(rtsp::decode_g711(pt, rtp.data() + off, len, pcm) && pcm.size() == 3);
    ACHECK(pcm[0] == 0 && pcm[1] == 32124 && pcm[2] == -32124);
    std::vector<uint8_t> ext = {0xB0, 8, 0, 1, 0, 0, 0, 0, 1, 2, 3, 4, 0xBE, 0xDE, 0, 1, 1, 2, 3, 4, 0xD5, 0xD5, 0, 2};
    ACHECK(rtsp::rtp_payload(ext.data(), ext.size(), pt, off, len) && pt == 8 && off == 20 && len == 2);
    ACHECK(!rtsp::decode_g711(96, ext.data(), 2, pcm));                // only G.711 is offered
    std::vector<uint8_t> bad = {0x90, 8, 0, 1, 0, 0, 0, 0, 1, 2, 3, 4, 0xBE, 0xDE, 0, 9};
    ACHECK(!rtsp::rtp_payload(bad.data(), bad.size(), pt, off, len));
}

void test_rtsp_audio_helpers() {
    const std::string sdp = rtsp::sdp_audio_section();
    ACHECK(sdp.rfind("m=audio 0 RTP/AVP 8\r\n", 0) == 0);
    ACHECK(sdp.find("a=rtpmap:8 PCMA/8000\r\n") != std::string::npos);
    ACHECK(sdp.find("a=control:trackID=1\r\n") != std::string::npos);

    ACHECK(rtsp::track_from_url("rtsp://cam/ch0/trackID=1") == 1);
    ACHECK(rtsp::track_from_url("rtsp://cam/stream=0/trackID=0") == 0);
    ACHECK(rtsp::track_from_url("rtsp://cam/ch0") == 0);                 // single-track clients: the video

    uint8_t h[12];
    rtsp::rtp_header(h, rtsp::kPayloadPcma, false, 0x1234, 0x01020304, 0xAABBCCDD);
    ACHECK(h[0] == 0x80 && h[1] == 8 && h[2] == 0x12 && h[3] == 0x34);
    ACHECK(h[4] == 1 && h[7] == 4 && h[8] == 0xAA && h[11] == 0xDD);
    rtsp::rtp_header(h, 96, true, 0, 0, 0);
    ACHECK(h[1] == (0x80 | 96));

    int a = -1, b = -1;
    ACHECK(rtsp::interleaved_channels("RTP/AVP/TCP;unicast;interleaved=2-3", a, b) && a == 2 && b == 3);
    ACHECK(rtsp::interleaved_channels("RTP/AVP/TCP;interleaved=4;mode=play", a, b) && a == 4 && b == 5);
    ACHECK(!rtsp::interleaved_channels("RTP/AVP;unicast;client_port=5000-5001", a, b));

    // One 40 ms frame at 16 kHz becomes one 320-byte PCMA payload (8 kHz).
    std::vector<int16_t> frame(640, 1000);
    std::string pcma;
    http::audio_encode(http::AudioFormat::Alaw, 16000, reinterpret_cast<const uint8_t*>(frame.data()), frame.size() * 2, pcma);
    ACHECK(pcma.size() == 320 && (uint8_t)pcma[0] == audio::alaw_encode((int16_t)1000));
}

bool contains(const std::vector<uint8_t>& b, const char* four) {
    for (size_t i = 0; i + 4 <= b.size(); ++i) if (memcmp(&b[i], four, 4) == 0) return true;
    return false;
}
int count(const std::vector<uint8_t>& b, const char* four) {
    int n = 0;
    for (size_t i = 0; i + 4 <= b.size(); ++i) if (memcmp(&b[i], four, 4) == 0) ++n;
    return n;
}
uint32_t rd32(const std::vector<uint8_t>& b, size_t at) { return ((uint32_t)b[at] << 24) | ((uint32_t)b[at + 1] << 16) | ((uint32_t)b[at + 2] << 8) | b[at + 3]; }

void test_ogg_opus() {
    const char* v = "123456789";
    ACHECK(ogg::crc(reinterpret_cast<const uint8_t*>(v), 9) == 0x89a1897fu);   // independent reference value
    ogg::OpusWriter w(0x1234, 16000, 312);
    const std::string h = w.headers();
    ACHECK(h.compare(0, 4, "OggS") == 0 && (uint8_t)h[5] == 0x02);          // BOS
    ACHECK(h.find("OpusHead") != std::string::npos && h.find("OpusTags") != std::string::npos);
    const size_t head = h.find("OpusHead");
    ACHECK((uint8_t)h[head + 8] == 1 && (uint8_t)h[head + 9] == 1);           // version 1, mono
    ACHECK((uint8_t)h[head + 10] == (312 & 0xff) && (uint8_t)h[head + 11] == (312 >> 8));
    // Every page's CRC checks out when recomputed with the field zeroed.
    auto crc_ok = [](std::string page) {
        uint32_t stored = 0;
        for (int i = 0; i < 4; ++i) stored |= (uint32_t)(uint8_t)page[22 + i] << (8 * i);
        for (int i = 0; i < 4; ++i) page[22 + i] = 0;
        return ogg::crc(reinterpret_cast<const uint8_t*>(page.data()), page.size()) == stored;
    };
    const size_t second = h.find("OggS", 4);
    ACHECK(second != std::string::npos && crc_ok(h.substr(0, second)) && crc_ok(h.substr(second)));
    const std::string p1 = w.packet(std::vector<uint8_t>(40, 7), 960);
    const std::string p2 = w.packet(std::vector<uint8_t>(300, 9), 960);   // > 255: two lacing values
    ACHECK(crc_ok(p1) && crc_ok(p2));
    auto granule = [](const std::string& pg) { uint64_t g = 0; for (int i = 0; i < 8; ++i) g |= (uint64_t)(uint8_t)pg[6 + i] << (8 * i); return g; };
    ACHECK(granule(p1) == 960 && granule(p2) == 1920);
    ACHECK((uint8_t)p2[26] == 2 && (uint8_t)p2[27] == 255 && (uint8_t)p2[28] == 45);
    auto seqno = [](const std::string& pg) { return (uint32_t)(uint8_t)pg[18] | ((uint32_t)(uint8_t)pg[19] << 8); };
    ACHECK(seqno(p1) == 2 && seqno(p2) == 3);                                  // after the two header pages
}

void test_fmp4_audio() {
    fmp4::AudioTrack aac; aac.codec = fmp4::AudioTrack::Aac; aac.track_id = 1; aac.sample_rate = 16000; aac.asc = audio::aac_asc(16000, 1);
    ACHECK(aac.asc.size() == 2 && aac.asc[0] == 0x14 && aac.asc[1] == 0x08);  // AAC-LC, 16 kHz, mono
    const std::vector<uint8_t> a = fmp4::audio_init_segment(aac);
    ACHECK(contains(a, "ftyp") && contains(a, "mp4a") && contains(a, "esds") && contains(a, "smhd") && contains(a, "soun"));
    ACHECK(count(a, "trak") == 1 && count(a, "trex") == 1);
    bool asc_found = false;
    for (size_t i = 0; i + 3 < a.size(); ++i) if (a[i] == 0x05 && a[i + 1] == 2 && a[i + 2] == 0x14 && a[i + 3] == 0x08) asc_found = true;
    ACHECK(asc_found);                                                         // DecoderSpecificInfo carries the ASC
    fmp4::AudioTrack op; op.codec = fmp4::AudioTrack::Opus; op.sample_rate = 8000; op.pre_skip = 312;
    ACHECK(op.timescale() == 48000 && aac.timescale() == 16000);
    const std::vector<uint8_t> sps = {0x67, 0x64, 0x00, 0x28, 0xac}, pps = {0x68, 0xee, 0x3c, 0x80};
    const std::vector<uint8_t> av = fmp4::init_segment(sps, pps, 1920, 1080, 90000, &op);
    ACHECK(count(av, "trak") == 2 && count(av, "trex") == 2 && contains(av, "avc1") && contains(av, "Opus") && contains(av, "dOps"));
    const std::vector<uint8_t> vo = fmp4::init_segment(sps, pps, 1920, 1080, 90000);
    ACHECK(count(vo, "trak") == 1 && !contains(vo, "soun"));                  // unchanged without audio
    const std::vector<uint8_t> f = fmp4::fragment(7, 4800, 960, std::vector<uint8_t>(20, 1), true, 2);
    size_t tfhd = 0; for (size_t i = 0; i + 4 <= f.size(); ++i) if (memcmp(&f[i], "tfhd", 4) == 0) { tfhd = i; break; }
    ACHECK(tfhd && rd32(f, tfhd + 8) == 2);                                    // track id 2
}

void test_encoders() {
    ACHECK(!audio::make_encoder("vorbis", 16000));
    if (!audio::codecs_built()) {                              // a build without CODECS says so
        ACHECK(!audio::make_aac_encoder(16000) && !audio::make_opus_encoder(8000));
        return;
    }
    std::vector<int16_t> tone(16000);
    for (size_t i = 0; i < tone.size(); ++i) tone[i] = (int16_t)(8000 * std::sin(2 * M_PI * 440 * (double)i / 16000));
    auto aac = audio::make_encoder("mp4a.40.2", 16000);
    ACHECK(aac && std::string(aac->codec()) == "mp4a.40.2" && aac->timescale() == 16000);
    std::vector<audio::EncodedFrame> fr;
    for (size_t off = 0; aac && off < tone.size(); off += 640) aac->encode(tone.data() + off, 640, fr);   // 40 ms frames, as the microphone delivers
    ACHECK(fr.size() >= 12 && fr.size() <= 16);                // 1 s = 15.6 frames of 1024, minus the encoder delay
    bool dur_ok = true, nonempty = true;
    for (const auto& e : fr) { dur_ok = dur_ok && e.duration == 1024; nonempty = nonempty && !e.data.empty() && e.data.size() < 1024; }
    ACHECK(dur_ok && nonempty);
    ACHECK(aac && aac->config().size() == 2 && aac->config()[0] == 0x14);

    std::vector<int16_t> t8(8000);
    for (size_t i = 0; i < t8.size(); ++i) t8[i] = (int16_t)(8000 * std::sin(2 * M_PI * 440 * (double)i / 8000));
    auto op = audio::make_encoder("opus", 8000);
    ACHECK(op && op->timescale() == 48000 && op->pre_skip() > 0);
    std::vector<audio::EncodedFrame> of;
    for (size_t off = 0; op && off < t8.size(); off += 320) op->encode(t8.data() + off, 320, of);
    ACHECK(of.size() == 50);                                    // 1 s = 50 x 20 ms
#ifdef MACHINO_CODECS
    // Round trip through the real decoder: the tone comes back.
    int err = 0;
    OpusDecoder* d = opus_decoder_create(8000, 1, &err);
    ACHECK(d && err == OPUS_OK);
    double energy = 0; size_t n = 0;
    for (size_t i = 0; d && i < of.size(); ++i) {
        int16_t out[960];
        const int got = opus_decode(d, of[i].data.data(), (opus_int32)of[i].data.size(), out, 960, 0);
        if (got <= 0) { energy = -1; break; }
        if (i >= 10) for (int k = 0; k < got; ++k) { energy += (double)out[k] * out[k]; ++n; }
    }
    const double rms = n ? std::sqrt(energy / (double)n) : 0;
    ACHECK(rms > 3000 && rms < 8000);                           // a 8000-peak sine has rms ~5657
    if (d) opus_decoder_destroy(d);
#endif
}

} // namespace

void run_audio_tests() {
    test_ogg_opus();
    test_fmp4_audio();
    test_encoders();
    test_rtsp_backchannel_helpers();
    test_rtsp_audio_helpers();
    test_speaker();
    test_play_body();
    test_majestic_unbuilt_urls();
    test_http_audio_stream();
    test_audio_test_helpers();
    test_majestic_audio_mapping();
    test_g711_reference();
    test_volume_curves();
    test_audio_config();
    test_service_refusals();
    test_service_lifecycle();
    test_service_disable_drops_listeners();
}
