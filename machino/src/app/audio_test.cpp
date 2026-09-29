#include "app/audio_test.hpp"
#include "core/audio/audio_service.hpp"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

namespace machino { namespace app {

static void put16(std::vector<uint8_t>& v, uint16_t x) { v.push_back((uint8_t)x); v.push_back((uint8_t)(x >> 8)); }
static void put32(std::vector<uint8_t>& v, uint32_t x) { put16(v, (uint16_t)x); put16(v, (uint16_t)(x >> 16)); }
static uint16_t get16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t get32(const uint8_t* p) { return (uint32_t)get16(p) | ((uint32_t)get16(p + 2) << 16); }

std::vector<uint8_t> wav_header(int rate, uint32_t samples) {
    std::vector<uint8_t> h;
    const uint32_t data = samples * 2;
    h.insert(h.end(), {'R', 'I', 'F', 'F'}); put32(h, 36 + data);
    h.insert(h.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '}); put32(h, 16);
    put16(h, 1); put16(h, 1);                      // PCM, mono
    put32(h, (uint32_t)rate); put32(h, (uint32_t)rate * 2);
    put16(h, 2); put16(h, 16);                     // block align, bits
    h.insert(h.end(), {'d', 'a', 't', 'a'}); put32(h, data);
    return h;
}

bool wav_parse(const std::vector<uint8_t>& f, int& rate, std::vector<int16_t>& pcm, std::string& err) {
    if (f.size() < 12 || memcmp(f.data(), "RIFF", 4) || memcmp(f.data() + 8, "WAVE", 4)) { err = "not a RIFF/WAVE file"; return false; }
    bool fmt_ok = false;
    for (size_t p = 12; p + 8 <= f.size(); ) {
        const uint32_t len = get32(&f[p + 4]);
        const size_t body = p + 8;
        if (body + len > f.size()) { err = "truncated chunk"; return false; }
        if (!memcmp(&f[p], "fmt ", 4)) {
            if (len < 16) { err = "short fmt chunk"; return false; }
            if (get16(&f[body]) != 1 || get16(&f[body + 2]) != 1 || get16(&f[body + 14]) != 16) {
                err = "only 16-bit mono PCM is played"; return false;
            }
            rate = (int)get32(&f[body + 4]);
            fmt_ok = true;
        } else if (!memcmp(&f[p], "data", 4)) {
            if (!fmt_ok) { err = "data before fmt"; return false; }
            pcm.resize(len / 2);
            if (!pcm.empty()) memcpy(pcm.data(), &f[body], pcm.size() * 2);
            return true;
        }
        p = body + len + (len & 1);
    }
    err = "no data chunk";
    return false;
}

void level_dbfs(const std::vector<int16_t>& pcm, double& rms_db, double& peak_db) {
    double sum = 0; int peak = 0;
    for (int16_t s : pcm) { sum += (double)s * s; const int a = s < 0 ? -(int)s : s; if (a > peak) peak = a; }
    const double rms = pcm.empty() ? 0 : std::sqrt(sum / (double)pcm.size());
    rms_db  = rms  > 0 ? 20.0 * std::log10(rms / 32768.0) : -120.0;
    peak_db = peak > 0 ? 20.0 * std::log10((double)peak / 32768.0) : -120.0;
}

void sine(std::vector<int16_t>& out, size_t n, int rate, int hz, double amp, double& phase) {
    out.resize(n);
    const double step = 2.0 * M_PI * hz / rate;
    for (size_t i = 0; i < n; ++i) {
        out[i] = (int16_t)std::lround(std::sin(phase) * amp * 32767.0);
        phase += step;
        if (phase > 2.0 * M_PI) phase -= 2.0 * M_PI;
    }
}

static int64_t now_ms() { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000; }

static void usage() {
    fprintf(stderr,
        "usage: machino --audio-test [--rate 8000|16000] [--volume 0..100] [--gain 0..31] MODE\n"
        "  bias   [seconds=60]           microphone on, level once a second (measure MICBIAS now)\n"
        "  record <file.wav> [seconds=5] microphone -> WAV\n"
        "  tone   [seconds=10] [hz=1000] sine on the output (measure AC between OUT and GND)\n"
        "  play   <file.wav>             16-bit mono WAV at 8/16 kHz -> output\n"
        "Stop the daemon first (streamerctl stop): the codec has one owner.\n");
}

static int run_in(const AudioInFactory& in, const AudioParams& p, int seconds, FILE* wav) {
    std::unique_ptr<IAudioIn> mic = in ? in(p) : nullptr;
    if (!mic) { fprintf(stderr, "audio-test: the microphone did not open (see the log above)\n"); return 3; }
    fprintf(stderr, "audio-test: microphone ON at %d Hz for %d s%s\n", p.sample_rate, seconds,
            wav ? "" : " - MICBIAS should now be present on the mic pin");
    std::vector<int16_t> pcm, second;
    uint32_t total = 0;
    int64_t t_end = now_ms() + (int64_t)seconds * 1000, t_line = now_ms() + 1000;
    unsigned frames = 0, misses = 0;
    while (now_ms() < t_end) {
        int64_t pts = 0;
        if (!mic->read(pcm, pts, 200)) { ++misses; continue; }
        ++frames;
        second.insert(second.end(), pcm.begin(), pcm.end());
        if (wav) { fwrite(pcm.data(), 2, pcm.size(), wav); total += (uint32_t)pcm.size(); }
        if (now_ms() >= t_line) {
            double rms, peak; level_dbfs(second, rms, peak);
            fprintf(stderr, "  level rms %6.1f dBFS  peak %6.1f dBFS  (%u frames, %u misses)\n", rms, peak, frames, misses);
            second.clear(); t_line += 1000;
        }
    }
    if (wav) {
        const std::vector<uint8_t> h = wav_header(p.sample_rate, total);
        fseek(wav, 0, SEEK_SET); fwrite(h.data(), 1, h.size(), wav);
        fprintf(stderr, "audio-test: %u samples written\n", total);
    }
    if (frames == 0) { fprintf(stderr, "audio-test: the codec delivered NO frames\n"); return 4; }
    return 0;
}

int run_audio_test(int argc, char** argv, int first, const AudioInFactory& in, const AudioOutFactory& out) {
    AudioParams p;
    int volume = 60;          // percent; the stock-calibrated curves map it
    for (; first < argc && !strncmp(argv[first], "--", 2); ++first) {
        if (first + 1 >= argc) { usage(); return 2; }
        const int v = atoi(argv[first + 1]);
        if (!strcmp(argv[first], "--rate")) p.sample_rate = v;
        else if (!strcmp(argv[first], "--volume")) volume = v;
        else if (!strcmp(argv[first], "--gain")) p.gain = v;
        else { usage(); return 2; }
        ++first;
    }
    if (first >= argc) { usage(); return 2; }
    const std::string mode = argv[first++];
    auto arg = [&](int i, int def) { return first + i < argc ? atoi(argv[first + i]) : def; };

    if (mode == "bias" || mode == "record") {
        p.volume = audio::input_volume_to_vendor(volume);
        if (mode == "bias") return run_in(in, p, arg(0, 60), nullptr);
        if (first >= argc) { usage(); return 2; }
        FILE* f = fopen(argv[first], "wb");
        if (!f) { fprintf(stderr, "audio-test: cannot write %s\n", argv[first]); return 1; }
        const std::vector<uint8_t> h = wav_header(p.sample_rate, 0);
        fwrite(h.data(), 1, h.size(), f);                  // placeholder, rewritten with the real size
        const int rc = run_in(in, p, arg(1, 5), f);
        fclose(f);
        return rc;
    }
    if (mode == "tone" || mode == "play") {
        std::vector<int16_t> clip;
        if (mode == "play") {
            if (first >= argc) { usage(); return 2; }
            FILE* f = fopen(argv[first], "rb");
            if (!f) { fprintf(stderr, "audio-test: cannot read %s\n", argv[first]); return 1; }
            std::vector<uint8_t> file;
            uint8_t buf[4096]; size_t n;
            while ((n = fread(buf, 1, sizeof buf, f)) > 0) file.insert(file.end(), buf, buf + n);
            fclose(f);
            std::string err;
            if (!wav_parse(file, p.sample_rate, clip, err)) { fprintf(stderr, "audio-test: %s: %s\n", argv[first], err.c_str()); return 1; }
        }
        p.volume = audio::output_volume_to_vendor(volume);
        std::unique_ptr<IAudioOut> spk = out ? out(p) : nullptr;
        if (!spk) { fprintf(stderr, "audio-test: the output did not open (see the log above)\n"); return 3; }
        if (mode == "play") {
            fprintf(stderr, "audio-test: playing %zu samples at %d Hz\n", clip.size(), p.sample_rate);
            if (!spk->write(clip.data(), clip.size())) { fprintf(stderr, "audio-test: write failed\n"); return 4; }
            spk->drain(0);
            return 0;
        }
        const int seconds = arg(0, 10), hz = arg(1, 1000);
        fprintf(stderr, "audio-test: %d Hz sine on the output for %d s - measure AC between OUT and GND now\n", hz, seconds);
        double phase = 0;
        const size_t chunk = (size_t)p.samples_per_frame();
        const int64_t t_end = now_ms() + (int64_t)seconds * 1000;
        while (now_ms() < t_end) {
            sine(clip, chunk, p.sample_rate, hz, 0.5, phase);   // -6 dBFS: loud enough to measure, no clipping
            if (!spk->write(clip.data(), clip.size())) { fprintf(stderr, "audio-test: write failed\n"); return 4; }
        }
        spk->drain(0);
        return 0;
    }
    usage();
    return 2;
}

}} // namespace machino::app
