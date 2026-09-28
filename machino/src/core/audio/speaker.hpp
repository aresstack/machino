// Machino core: the speaker as a queue of clips.
//
// play() only QUEUES: the caller is the HTTP poll loop, which must never wait
// for audio to come out of a speaker. One playback thread opens the output
// through the platform when there is something to play, writes the clips in
// order, waits until the device has played them out, and closes the output
// again grace_ms after the queue ran dry ("no clip, no speaker").
//
// The device runs at the rate of the clip it plays; a clip at another rate
// than the open device closes and reopens it. The queue is bounded in
// samples, so a client cannot park megabytes of audio in RAM.
#pragma once
#include "core/result.hpp"
#include "ports/iaudio.hpp"
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace machino { namespace audio {

struct SpeakerStats {
    bool        enabled = false;
    bool        open = false;          // the codec output is up right now
    bool        playing = false;       // a clip is being written
    int         queued_ms = 0;         // audio waiting behind the current clip
    unsigned    clips = 0;             // clips played to the end since boot
    unsigned    dropped = 0;           // clips discarded (switched off, write error)
    std::string last_error;
};

class Speaker {
public:
    using OutFactory = std::function<std::unique_ptr<IAudioOut>(const AudioParams&)>;

    // Upper bound for everything queued, in seconds of audio at the clip's rate.
    static const int kMaxQueueSeconds = 70;   // one maximal /play_audio body at 8 kHz (~65 s) fits

    // `out` may be empty: this platform has no output, play() says so.
    Speaker(OutFactory out, bool enabled, int volume_percent, int grace_ms);
    ~Speaker();
    Speaker(const Speaker&) = delete;
    Speaker& operator=(const Speaker&) = delete;

    bool available() const { return (bool)factory_; }

    // Queue mono s16 samples at `rate` (8000|16000). Returns at once.
    // Unsupported: no output here. Busy: switched off, or the queue is full.
    Result play(std::vector<int16_t> pcm, int rate, std::string& why);

    // Off drops the queue and cuts a clip that is playing.
    void set_enabled(bool on);
    void set_volume(int percent);            // live on an open output
    // Blocks until everything queued has played (or timeout); for tests and
    // the command-line tool, never for the HTTP path.
    bool wait_idle(int timeout_ms);

    SpeakerStats stats() const;
    void shutdown();

private:
    struct Clip { std::vector<int16_t> pcm; int rate; };
    void loop();

    OutFactory               factory_;
    mutable std::mutex       m_;
    std::condition_variable  cv_;
    std::deque<Clip>         queue_;
    size_t                   queued_samples_ = 0;
    int                      queued_rate_ = 8000;   // for the queued_ms figure
    bool                     enabled_;
    int                      volume_;
    int                      grace_ms_;
    bool                     quit_ = false;
    bool                     cut_ = false;          // abandon the clip being written
    bool                     open_ = false, playing_ = false;
    unsigned                 clips_ = 0, dropped_ = 0;
    std::string              last_error_;
    IAudioOut*               dev_ = nullptr;        // valid while open_, for live volume
    std::thread              thread_;
};

}} // namespace machino::audio
