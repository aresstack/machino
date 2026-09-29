// Port: audio capture (microphone) and playback (speaker / line out). The core
// sees 16-bit little-endian mono PCM in fixed-size frames; which codec pins
// carry it, the vendor frame structures and the device/channel numbers are
// the adapter's business.
//
// Like the JPEG encoder the instances are ephemeral: the AudioService creates
// the input when the first listener arrives and destroys it after a short
// grace, so a camera nobody listens to keeps its codec input path down
// ("no consumer, no pipeline"). Neither direction needs the sensor or ISP.
#pragma once
#include "core/result.hpp"
#include <cstddef>
#include <cstdint>
#include <vector>

namespace machino {

struct AudioParams {
    int sample_rate = 8000;   // Hz; the adapter refuses rates it cannot run
    int frame_ms    = 40;     // samples per frame = sample_rate * frame_ms / 1000
    int volume      = 60;     // vendor volume scale, -30 (mute) .. 120; 60 = unity
    int gain        = -1;     // analog gain 0..31, -1 = leave the driver default
    // No HPF/NS/AGC/AEC knobs on purpose: on Ingenic those live in the vendor
    // libaudioProcess.so, which libimp dlopen()s and which OpenIPC does not
    // ship (docs/t40nn-stock-vs-openipc-media-stack.md). Plain capture and
    // playback do not load it.

    int samples_per_frame() const { return sample_rate * frame_ms / 1000; }
};

class IAudioIn {
public:
    virtual ~IAudioIn() = default;
    // Blocks up to timeout_ms for the next frame. `pcm` is replaced with one
    // frame of mono s16 samples; `pts_us` is the capture time in the
    // platform's timestamp domain.
    virtual Result read(std::vector<int16_t>& pcm, int64_t& pts_us, int timeout_ms) = 0;
    // Live, while capturing. Unsupported when the platform has no such knob.
    virtual Result set_volume(int vol) { (void)vol; return Result::unsupported(); }
    virtual Result set_gain(int gain)  { (void)gain; return Result::unsupported(); }
};

class IAudioOut {
public:
    virtual ~IAudioOut() = default;
    // Queues mono s16 samples at the output's sample rate. Blocks while the
    // device queue is full, so a caller feeding a file paces itself.
    virtual Result write(const int16_t* pcm, size_t samples) = 0;
    // Waits until everything written has left the device (or timeout).
    virtual Result drain(int timeout_ms) { (void)timeout_ms; return Result::ok(); }
    virtual Result set_volume(int vol) { (void)vol; return Result::unsupported(); }
    virtual int    sample_rate() const = 0;
};

} // namespace machino
