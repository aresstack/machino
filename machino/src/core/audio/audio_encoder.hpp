// Machino core: compressed audio for the browser - AAC-LC (libfaac) and Opus
// (libopus), the two codecs the stock WebUI's MSE player asks for
// (`&audio=opus,mp4a.40.2`) and majestic serves as /audio.m4a and /audio.opus.
//
// One encoder per consumer, fed the microphone's 40 ms s16 frames; it
// re-blocks them into the codec's own frame size (AAC 1024 samples, Opus 20 ms)
// and hands back whole encoded frames with their duration in the codec's
// timescale (AAC: the sample rate, Opus: 48 kHz as RFC 7845 fixes it).
//
// The libraries are optional (CODECS=<dir> at build time, see
// tools/fetch-codecs.sh). Without them the factories return nullptr and the
// endpoints say so.
#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace machino { namespace audio {

struct EncodedFrame {
    std::vector<uint8_t> data;
    uint32_t duration = 0;          // in timescale() units
};

class AudioEncoder {
public:
    virtual ~AudioEncoder() = default;
    virtual const char* codec() const = 0;          // "mp4a.40.2" | "opus" - the MSE codec name
    virtual int  sample_rate() const = 0;           // input rate
    virtual uint32_t timescale() const = 0;
    // AAC: the 2-byte AudioSpecificConfig. Opus: empty.
    virtual std::vector<uint8_t> config() const { return {}; }
    // Opus: samples at 48 kHz to discard at the start (encoder lookahead).
    virtual uint16_t pre_skip() const { return 0; }
    // Appends every frame that became complete.
    virtual bool encode(const int16_t* pcm, size_t n, std::vector<EncodedFrame>& out) = 0;
};

bool codecs_built();
// nullptr when the library is not built in or refused the parameters.
std::unique_ptr<AudioEncoder> make_aac_encoder(int sample_rate, int bitrate_bps = 32000);
std::unique_ptr<AudioEncoder> make_opus_encoder(int sample_rate, int bitrate_bps = 24000);
// "opus" / "mp4a.40.2" -> encoder; anything else nullptr.
std::unique_ptr<AudioEncoder> make_encoder(const std::string& codec, int sample_rate);

// AAC AudioSpecificConfig for AAC-LC mono/stereo (pure, for the tests).
std::vector<uint8_t> aac_asc(int sample_rate, int channels);

}} // namespace machino::audio
