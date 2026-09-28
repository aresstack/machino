#include "core/audio/audio_service.hpp"
#include "core/log.hpp"
#include <ctime>
#include <unistd.h>

namespace machino { namespace audio {

static const char* MOD = "AUDIO";

// Consecutive failed reads (errors or timeouts at 100 ms each) after which
// the service SAYS that the codec delivers nothing. It keeps trying: a silent
// microphone is reported, not silently switched off.
static const unsigned kSilentReads = 50;
static const size_t   kListenerDepth = 8;     // frames a slow listener may lag (8 x 40 ms)

static int64_t mono_us() {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

static int interpolate(const int (&t)[11], int percent) {
    if (percent <= 0) return -30;
    if (percent >= 100) return t[10];
    const int i = percent / 10, f = percent % 10;
    return t[i] + (t[i + 1] - t[i]) * f / 10;
}

int input_volume_to_vendor(int percent) {
    static const int t[11] = {-30, 0, 15, 30, 40, 50, 60, 70, 80, 90, 100};
    return interpolate(t, percent);
}

int output_volume_to_vendor(int percent) {
    static const int t[11] = {-30, 0, 15, 30, 45, 60, 70, 75, 80, 85, 90};
    return interpolate(t, percent);
}

AudioService::AudioService(const AudioConfig& cfg, InFactory in)
    : in_factory_(std::move(in)), cfg_(cfg) {}

AudioService::~AudioService() { shutdown(); }

AudioParams AudioService::params_locked() const {
    AudioParams p;
    p.sample_rate = cfg_.srate;
    p.frame_ms = 40;
    p.volume = input_volume_to_vendor(cfg_.volume);
    p.gain = cfg_.gain;
    return p;
}

// The capture thread has decided to stop (running_ is false) but may still be
// closing the device. Joining here guarantees the old device is gone before a
// new one is opened. The thread takes no lock after it set running_=false.
void AudioService::join_stopped_locked() {
    if (thread_.joinable()) thread_.join();
}

std::shared_ptr<Sink> AudioService::listen(std::string& why) {
    std::lock_guard<std::mutex> lk(m_);
    if (shutdown_)         { why = "shutting down"; return nullptr; }
    if (!in_factory_)      { why = "this platform has no audio input"; return nullptr; }
    if (!cfg_.enabled)     { why = "audio is disabled (audio.enabled=false)"; return nullptr; }
    if (!running_) {
        join_stopped_locked();
        const AudioParams p = params_locked();
        std::unique_ptr<IAudioIn> dev = in_factory_(p);
        if (!dev) {
            last_error_ = "the codec refused the microphone at " + std::to_string(p.sample_rate) + " Hz";
            why = last_error_;
            return nullptr;
        }
        in_ = std::move(dev);
        rate_ = p.sample_rate;
        running_ = true;
        quit_ = false;
        ++starts_;
        last_error_.clear();
        thread_ = std::thread(&AudioService::capture_loop, this);
        LOGI(MOD, "microphone opened for a listener (%d Hz)", rate_);
    }
    // Subscribed under the same lock the thread's stop decision takes, so the
    // thread can never see "no listener" between the open and this line.
    return hub_.subscribe(kListenerDepth);
}

void AudioService::unlisten(const std::shared_ptr<Sink>& s) {
    if (!s) return;
    s->close();
    hub_.unsubscribe(s);
}

void AudioService::capture_loop() {
    IAudioIn* dev = nullptr;
    int grace_ms = 0;
    {
        std::lock_guard<std::mutex> lk(m_);
        dev = in_.get();
        grace_ms = cfg_.grace_ms;
    }
    const size_t frame_bytes = (size_t)rate_ * 40 / 1000 * sizeof(int16_t);
    AuPool pool(kListenerDepth + 4, frame_bytes);
    std::vector<int16_t> pcm;
    uint32_t seq = 0;
    unsigned misses = 0;
    int64_t idle_since = -1;
    for (;;) {
        int64_t pts = 0;
        const Result r = dev->read(pcm, pts, 100);
        if (r && !pcm.empty()) {
            misses = 0;
            if (auto au = pool.acquire()) {
                const uint8_t* b = reinterpret_cast<const uint8_t*>(pcm.data());
                au->data.assign(b, b + pcm.size() * sizeof(int16_t));   // s16le: the target is little-endian
                au->pts_us = pts;
                au->fetched_us = mono_us();
                au->key = true;          // every PCM frame is independently decodable
                au->seq = seq++;
                hub_.publish(au);
            }
            ++frames_;
        } else {
            if (r.status != Status::Timeout) { ++read_errors_; usleep(10000); }   // never spin on a failing device
            if (++misses == kSilentReads) {
                std::lock_guard<std::mutex> lk(m_);
                last_error_ = "the codec delivers no audio frames";
                LOGW(MOD, "%s (%u reads) - still trying", last_error_.c_str(), misses);
            }
        }

        std::unique_ptr<IAudioIn> closing;
        {
            std::lock_guard<std::mutex> lk(m_);
            bool stop = quit_ || !cfg_.enabled;
            if (!stop) {
                if (hub_.consumers() == 0) {
                    const int64_t now = mono_us();
                    if (idle_since < 0) idle_since = now;
                    if (now - idle_since >= (int64_t)grace_ms * 1000) stop = true;
                } else idle_since = -1;
            }
            if (stop) { running_ = false; closing = std::move(in_); }
        }
        if (closing) {
            closing.reset();             // IMP_AI_DisableChn/Disable, outside the lock
            LOGI(MOD, "microphone closed (no listener)");
            return;
        }
    }
}

int AudioService::sample_rate() const {
    std::lock_guard<std::mutex> lk(m_);
    return running_ ? rate_ : cfg_.srate;
}

AudioConfig AudioService::config() const {
    std::lock_guard<std::mutex> lk(m_);
    return cfg_;
}

Result AudioService::set_enabled(bool on) {
    std::lock_guard<std::mutex> lk(m_);
    cfg_.enabled = on;
    // Listeners are dropped NOW, not at the next frame: turning the
    // microphone off must not leave a stream open that merely goes quiet.
    if (!on) hub_.close_all();
    return Result::ok();
}

Result AudioService::set_volume(int percent) {
    std::lock_guard<std::mutex> lk(m_);
    cfg_.volume = percent;
    return in_ ? in_->set_volume(input_volume_to_vendor(percent)) : Result::ok();
}

Result AudioService::set_gain(int gain) {
    std::lock_guard<std::mutex> lk(m_);
    cfg_.gain = gain;
    if (gain < 0 || !in_) return Result::ok();   // -1: driver default, applies at the next open
    return in_->set_gain(gain);
}

Result AudioService::set_sample_rate(int hz) {
    if (hz != 8000 && hz != 16000) return Result::unsupported();
    std::lock_guard<std::mutex> lk(m_);
    cfg_.srate = hz;
    return Result::ok();
}

Result AudioService::set_output_enabled(bool on) {
    std::lock_guard<std::mutex> lk(m_);
    cfg_.output_enabled = on;
    return Result::ok();
}

Result AudioService::set_output_volume(int percent) {
    std::lock_guard<std::mutex> lk(m_);
    cfg_.output_volume = percent;
    return Result::ok();
}

AudioStats AudioService::stats() const {
    std::lock_guard<std::mutex> lk(m_);
    AudioStats s;
    s.enabled = cfg_.enabled;
    s.capturing = running_;
    s.listeners = (int)hub_.consumers();
    s.sample_rate = running_ ? rate_ : cfg_.srate;
    s.starts = starts_;
    s.frames = frames_;
    s.read_errors = read_errors_;
    s.last_error = last_error_;
    return s;
}

void AudioService::shutdown() {
    std::thread t;
    {
        std::lock_guard<std::mutex> lk(m_);
        shutdown_ = true;
        quit_ = true;
        hub_.close_all();
        t = std::move(thread_);
    }
    if (t.joinable()) t.join();
}

}} // namespace machino::audio
