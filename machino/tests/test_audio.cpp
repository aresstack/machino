// Audio: G.711 against reference values, the volume curves, the audio config
// keys, and the AudioService lifecycle ("no listener, no microphone") against
// a fake codec input.
#include "app/audio_test.hpp"
#include "app/compat/majestic_migrate.hpp"
#include "app/compat/majestic_webui.hpp"
#include "app/http/audio_stream.hpp"
#include "core/audio/audio_service.hpp"
#include "core/audio/g711.hpp"
#include "core/json.hpp"
#include "core/config.hpp"

#include <atomic>
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
    ACHECK(audio_format_for_path("/audio.opus") == AudioFormat::None);     // not built: answered 501 by the server, not here
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
    ACHECK(ma && !ma->get("outputEnabled")->as_bool());          // no speaker path: never claimed

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

} // namespace

void run_audio_tests() {
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
