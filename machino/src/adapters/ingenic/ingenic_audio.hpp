// Ingenic adapter: the T40 inner codec through IMP_AI (microphone) and IMP_AO
// (speaker / line out), device 0 channel 0 both ways - the same pair the stock
// firmware (ovfs_boardsystem) drives on this board.
//
// Which physical pins carry the signal is decided by the kernel's audio.ko
// (mono_channel, spk_gpio), not here. The stock firmware loads it with
// `spk_gpio=-1 mono_channel=1` and still plays through IMP_AO, so spk_gpio=-1
// does NOT mean "no output": it only means the driver switches no amplifier
// pin. See docs/ap20-audio-talkback.md.
//
// Each object owns its direction for its lifetime (RAII): the constructor
// path (create) brings the device and channel up with every step checked, the
// destructor takes down exactly what was brought up.
#pragma once
#include "ports/iaudio.hpp"
#include <memory>
#include <mutex>

namespace machino { namespace ingenic {

// The rates the inner codec accepts for both directions. Anything else is
// refused up front instead of being tried on the hardware.
bool audio_rate_supported(int hz);

class IngenicAudioIn final : public IAudioIn {
public:
    static std::unique_ptr<IngenicAudioIn> create(const AudioParams& p);
    ~IngenicAudioIn() override;

    Result read(std::vector<int16_t>& pcm, int64_t& pts_us, int timeout_ms) override;
    Result set_volume(int vol) override;
    Result set_gain(int gain) override;

private:
    IngenicAudioIn() = default;
    bool dev_enabled_ = false, chn_enabled_ = false;
    std::mutex m_;               // volume/gain against a running GetFrame
};

class IngenicAudioOut final : public IAudioOut {
public:
    static std::unique_ptr<IngenicAudioOut> create(const AudioParams& p);
    ~IngenicAudioOut() override;

    Result write(const int16_t* pcm, size_t samples) override;
    Result drain(int timeout_ms) override;
    Result set_volume(int vol) override;
    int    sample_rate() const override { return rate_; }

private:
    IngenicAudioOut() = default;
    bool dev_enabled_ = false, chn_enabled_ = false;
    int  rate_ = 0;
    int  period_ = 0;            // samples per IMP_AO_SendFrame (the configured numPerFrm)
};

}} // namespace machino::ingenic
