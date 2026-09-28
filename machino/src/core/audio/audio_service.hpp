// Machino core: the microphone as a demand-driven stream.
//
// "No consumer, no pipeline", the audio way: the codec input exists only
// while somebody listens. The first listener creates the IAudioIn through the
// platform and starts one capture thread; frames go into a StreamHub (one
// AccessUnit = one frame of s16le mono PCM, every frame a "key" frame, so a
// slow listener that drops simply resumes at the next frame). When the last
// listener leaves, the thread keeps the device open for grace_ms - a player
// that reconnects does not re-open the codec - and then closes it.
//
// Audio needs neither the sensor nor the ISP and never takes video demand.
// Transitions are serialised by one mutex that the frame path never holds
// across a blocking read.
#pragma once
#include "core/config.hpp"
#include "core/result.hpp"
#include "core/stream_hub.hpp"
#include "ports/iaudio.hpp"
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace machino { namespace audio {

// majestic's 0..100 volume onto the vendor's -30..120 scale (0.5 dB steps, 60
// = unity). The curves are the board vendor's own calibration, read off the
// stock firmware of the T40NN (appfs/audio_param.json, VolConfig, one point
// per 10 %), interpolated in between. 0 is mute.
int input_volume_to_vendor(int percent);
int output_volume_to_vendor(int percent);

struct AudioStats {
    bool        enabled = false;
    bool        capturing = false;     // the codec input is open right now
    int         listeners = 0;
    int         sample_rate = 0;
    unsigned    starts = 0;            // device opens since boot
    unsigned    frames = 0;            // frames published since boot
    unsigned    read_errors = 0;
    std::string last_error;
};

class AudioService {
public:
    using InFactory = std::function<std::unique_ptr<IAudioIn>(const AudioParams&)>;

    // `in` may be empty: the platform has no audio input, and every listen()
    // is refused with that reason.
    AudioService(const AudioConfig& cfg, InFactory in);
    ~AudioService();
    AudioService(const AudioService&) = delete;
    AudioService& operator=(const AudioService&) = delete;

    bool available() const { return (bool)in_factory_; }

    // A new listener. Opens the microphone if it is not open yet. nullptr with
    // `why` when audio is disabled, unavailable, or the device refused.
    std::shared_ptr<Sink> listen(std::string& why);
    void unlisten(const std::shared_ptr<Sink>& s);

    int sample_rate() const;
    AudioConfig config() const;

    // Live controls. set_enabled(false) closes every listener at once.
    Result set_enabled(bool on);
    Result set_volume(int percent);
    Result set_gain(int gain);
    // Stored; the running device keeps its rate until it is next opened
    // (after the last listener has gone and the grace has run out).
    Result set_sample_rate(int hz);
    // The speaker side is configuration only in this build: remembered and
    // reported, so the WebUI round-trips it, but nothing plays yet.
    Result set_output_enabled(bool on);
    Result set_output_volume(int percent);

    AudioStats stats() const;
    void shutdown();

private:
    void capture_loop();
    AudioParams params_locked() const;
    void join_stopped_locked();

    InFactory              in_factory_;
    StreamHub              hub_;
    mutable std::mutex     m_;
    AudioConfig            cfg_;
    std::unique_ptr<IAudioIn> in_;      // owned while capturing; moved out by the thread to close it
    std::thread            thread_;
    bool                   running_ = false;
    bool                   shutdown_ = false;
    int                    rate_ = 0;   // the rate the running device was opened at
    unsigned               starts_ = 0;
    std::string            last_error_;
    std::atomic<unsigned>  frames_{0}, read_errors_{0};
    std::atomic<bool>      quit_{false};
};

}} // namespace machino::audio
