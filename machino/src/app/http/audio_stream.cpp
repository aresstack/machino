#include "app/http/audio_stream.hpp"
#include "app/audio_test.hpp"
#include "core/audio/g711.hpp"
#include <cstring>

namespace machino { namespace http {

AudioFormat audio_format_for_path(const std::string& path) {
    if (path == "/audio.pcm") return AudioFormat::Pcm;
    if (path == "/audio.alaw" || path == "/audio.g711a") return AudioFormat::Alaw;
    if (path == "/audio.ulaw") return AudioFormat::Ulaw;
    if (path == "/audio.m4a") return AudioFormat::Aac;
    if (path == "/audio.opus") return AudioFormat::Opus;
    return AudioFormat::None;
}

const char* audio_format_name(AudioFormat f) {
    switch (f) {
        case AudioFormat::Pcm:  return "pcm";
        case AudioFormat::Alaw: return "alaw";
        case AudioFormat::Ulaw: return "ulaw";
        case AudioFormat::Aac:  return "aac";
        case AudioFormat::Opus: return "opus";
        case AudioFormat::None: break;
    }
    return "none";
}

const char* audio_codec_for(AudioFormat f) {
    return f == AudioFormat::Aac ? "mp4a.40.2" : f == AudioFormat::Opus ? "opus" : "";
}

int audio_wire_rate(AudioFormat f, int capture_rate) {
    if (f == AudioFormat::Opus) return 48000;       // RFC 7845: Opus is always 48 kHz on the wire
    return f == AudioFormat::Pcm || f == AudioFormat::Aac ? capture_rate : 8000;
}

std::string audio_stream_headers(AudioFormat f, int capture_rate) {
    std::string ct;
    switch (f) {
        // Not audio/L16: RFC 2586 makes L16 big-endian, and this stream is the
        // raw little-endian samples the codec produced.
        case AudioFormat::Pcm:  ct = "audio/pcm;rate=" + std::to_string(capture_rate) + ";channels=1;format=s16le"; break;
        case AudioFormat::Alaw: ct = "audio/PCMA"; break;       // IANA: 8 kHz implied
        case AudioFormat::Ulaw: ct = "audio/PCMU"; break;
        case AudioFormat::Aac:  ct = "audio/mp4; codecs=\"mp4a.40.2\""; break;
        case AudioFormat::Opus: ct = "audio/ogg; codecs=opus"; break;
        case AudioFormat::None: ct = "application/octet-stream"; break;
    }
    return "HTTP/1.1 200 OK\r\nContent-Type: " + ct +
           "\r\nX-Audio-Rate: " + std::to_string(audio_wire_rate(f, capture_rate)) +
           "\r\nX-Audio-Channels: 1\r\nCache-Control: no-store\r\nPragma: no-cache\r\n"
           "Access-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n";
}

size_t audio_stream_bytes(AudioFormat f, int capture_rate, int seconds) {
    size_t per_second = 0;
    switch (f) {
        case AudioFormat::Pcm:  per_second = (size_t)capture_rate * 2; break;
        case AudioFormat::Aac:  per_second = 6000; break;                 // 32 kbit/s + fMP4 framing
        case AudioFormat::Opus: per_second = 4000; break;                 // 24 kbit/s + Ogg framing
        default:                per_second = 8000; break;                 // G.711 at 8 kHz
    }
    return per_second * (size_t)(seconds > 0 ? seconds : 1);
}

void audio_encode(AudioFormat f, int capture_rate, const uint8_t* s16le, size_t bytes, std::string& out) {
    const size_t n = bytes / 2;
    if (f == AudioFormat::Pcm) { out.append(reinterpret_cast<const char*>(s16le), n * 2); return; }
    if (f != AudioFormat::Alaw && f != AudioFormat::Ulaw) return;
    // 16 kHz -> 8 kHz: average each pair. A two-tap box filter is a crude
    // anti-alias, but G.711 speech is band-limited to 3.4 kHz anyway.
    const size_t step = capture_rate == 16000 ? 2 : 1;
    const size_t start = out.size();
    out.resize(start + n / step);
    uint8_t* dst = reinterpret_cast<uint8_t*>(&out[start]);
    for (size_t i = 0, o = 0; i + step <= n; i += step, ++o) {
        int acc = 0;
        for (size_t k = 0; k < step; ++k) {
            int16_t s; std::memcpy(&s, s16le + (i + k) * 2, 2);   // unaligned-safe
            acc += s;
        }
        const int16_t v = (int16_t)(acc / (int)step);
        dst[o] = f == AudioFormat::Alaw ? audio::alaw_encode(v) : audio::ulaw_encode(v);
    }
}

bool play_body_to_pcm(const std::string& body, int raw_rate, std::vector<int16_t>& pcm, int& rate, std::string& err) {
    if (body.size() < 2) { err = "the body holds no samples"; return false; }
    if (body.size() >= 12 && body.compare(0, 4, "RIFF") == 0 && body.compare(8, 4, "WAVE") == 0) {
        const std::vector<uint8_t> file(body.begin(), body.end());
        if (!app::wav_parse(file, rate, pcm, err)) return false;
        if (pcm.empty()) { err = "the WAV file holds no samples"; return false; }
        return true;
    }
    rate = raw_rate;
    pcm.resize(body.size() / 2);                     // an odd trailing byte is not a sample
    std::memcpy(pcm.data(), body.data(), pcm.size() * 2);
    return true;
}

}} // namespace machino::http
