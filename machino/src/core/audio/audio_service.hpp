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
#include "core/audio/speaker.hpp"
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
    bool        monitoring = false;    // loopback (mic -> speaker) is running
    std::string last_error;
};

class AudioService {
public:
    using InFactory = std::function<std::unique_ptr<IAudioIn>(const AudioParams&)>;
    using OutFactory = Speaker::OutFactory;

    // `in` / `out` may be empty: the platform has no audio input / output,
    // and every listen() / play() is refused with that reason.
    AudioService(const AudioConfig& cfg, InFactory in, OutFactory out = nullptr);
    ~AudioService();
    AudioService(const AudioService&) = delete;
    AudioService& operator=(const AudioService&) = delete;

    bool available() const { return (bool)in_factory_; }
    bool output_available() const { return speaker_.available(); }

    // The speaker (majestic /play_audio): queues a clip, returns at once.
    Result play(std::vector<int16_t> pcm, int rate, std::string& why) { return speaker_.play(std::move(pcm), rate, why); }
    SpeakerStats speaker_stats() const { return speaker_.stats(); }
    bool wait_speaker_idle(int timeout_ms) { return speaker_.wait_idle(timeout_ms); }

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
    // The speaker switch and level, live.
    Result set_output_enabled(bool on);
    Result set_output_volume(int percent);

    // Loopback ("monitor"): one internal listener whose every frame is played
    // straight back on the speaker, so an operator hears the live microphone.
    // Needs both a microphone and a speaker; the caller enables audio.enabled
    // and audio.outputEnabled first. There is no echo canceller - with an open
    // speaker near the microphone this will feed back; that is the operator's
    // call, warned in the UI. off stops the pump and lets the codec close.
    Result set_monitor(bool on);
    bool   monitoring() const { return mon_running_.load(); }

    AudioStats stats() const;
    void shutdown();

private:
    void capture_loop();
    void monitor_loop();
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
    // Loopback pump, guarded by its own mutex (never taken with m_): the thread
    // uses the public listen()/unlisten()/play(), each of which takes m_ itself.
    std::mutex             mon_m_;
    std::thread            mon_thread_;
    std::atomic<bool>      mon_quit_{false};
    std::atomic<bool>      mon_running_{false};
    Speaker                speaker_;    // last: its thread stops first on destruction
};

}} // namespace machino::audio
