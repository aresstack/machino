// Application: majestic's plain HTTP audio streams. No container and no
// framing - the body is the sample stream itself, which is what a player
// told the format on its command line expects:
//
//   /audio.pcm    s16le mono at the capture rate (ffplay -f s16le -ar 8000 -ac 1)
//   /audio.alaw   G.711 A-law, 8 kHz               (ffplay -f alaw -ar 8000 -ac 1)
//   /audio.g711a  the same stream under majestic's other name
//   /audio.ulaw   G.711 mu-law, 8 kHz              (ffplay -f mulaw -ar 8000 -ac 1)
//
// G.711 is 8 kHz by definition, so a microphone running at 16 kHz is
// decimated 2:1 for those two; PCM keeps the capture rate and says which in
// its Content-Type. Pure functions, host-tested.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace machino { namespace http {

// Aac: /audio.m4a, fragmented MP4 (init + one moof/mdat per AAC frame).
// Opus: /audio.opus, Ogg Opus. Both need the optional encoders (CODECS=).
enum class AudioFormat { None = 0, Pcm, Alaw, Ulaw, Aac, Opus };

AudioFormat audio_format_for_path(const std::string& path);
const char* audio_format_name(AudioFormat f);
// The rate the wire carries for a capture at `capture_rate`.
int audio_wire_rate(AudioFormat f, int capture_rate);
// How many bytes of `f` at the wire rate `seconds` of audio are - the most a
// client's output buffer may hold before new frames are dropped (compressed
// formats: the encoder bit rate, generously).
size_t audio_stream_bytes(AudioFormat f, int capture_rate, int seconds);
std::string audio_stream_headers(AudioFormat f, int capture_rate);
// Compressed formats: the encoder's codec name ("mp4a.40.2" / "opus"), "" otherwise.
const char* audio_codec_for(AudioFormat f);
// One captured frame (s16le mono at capture_rate) appended to `out` as wire bytes
// (PCM and G.711 only; the compressed formats go through an encoder).
void audio_encode(AudioFormat f, int capture_rate, const uint8_t* s16le, size_t bytes, std::string& out);

// /play_audio: the body is what the stock WebUI sends - raw s16le mono at the
// camera's own rate (`raw_rate`, audio.srate), no container. A RIFF/WAVE body
// (16-bit mono) is accepted too and plays at its own rate. False with `err`
// for an empty or malformed body.
static const size_t kMaxPlayBodyBytes = 1024 * 1024;   // ~32 s at 16 kHz, ~65 s at 8 kHz
bool play_body_to_pcm(const std::string& body, int raw_rate, std::vector<int16_t>& pcm, int& rate, std::string& err);

}} // namespace machino::http
