#include "core/audio/speaker.hpp"
#include "core/audio/audio_service.hpp"
#include "core/log.hpp"
#include <algorithm>
#include <chrono>

namespace machino { namespace audio {

static const char* MOD = "SPEAKER";

Speaker::Speaker(OutFactory out, bool enabled, int volume_percent, int grace_ms)
    : factory_(std::move(out)), enabled_(enabled), volume_(volume_percent), grace_ms_(grace_ms) {
    if (factory_) thread_ = std::thread(&Speaker::loop, this);
}

Speaker::~Speaker() { shutdown(); }

Result Speaker::play(std::vector<int16_t> pcm, int rate, std::string& why) {
    std::lock_guard<std::mutex> lk(m_);
    if (!factory_) { why = "this platform has no audio output"; return Result::unsupported(); }
    if (quit_)     { why = "shutting down"; return Result::busy(); }
    if (!enabled_) { why = "the speaker is switched off (audio.outputEnabled is false)"; return Result::busy(); }
    if (rate != 8000 && rate != 16000) { why = "the speaker plays 8000 or 16000 Hz, not " + std::to_string(rate); return Result::unsupported(); }
    if (pcm.empty()) { why = "the clip is empty"; return Result::error(); }
    const size_t cap = (size_t)kMaxQueueSeconds * (size_t)rate;
    if (queued_samples_ + pcm.size() > cap) {
        why = "the speaker queue is full (" + std::to_string(kMaxQueueSeconds) + " s)";
        return Result::busy();
    }
    queued_samples_ += pcm.size();
    queued_rate_ = rate;
    queue_.push_back(Clip{std::move(pcm), rate});
    cv_.notify_all();
    return Result::ok();
}

void Speaker::set_enabled(bool on) {
    std::lock_guard<std::mutex> lk(m_);
    enabled_ = on;
    if (!on) {
        dropped_ += (unsigned)queue_.size();
        queue_.clear();
        queued_samples_ = 0;
        cut_ = playing_;          // a clip being written stops at its next chunk
    }
    cv_.notify_all();
}

void Speaker::set_volume(int percent) {
    std::lock_guard<std::mutex> lk(m_);
    volume_ = percent;
    if (dev_) dev_->set_volume(output_volume_to_vendor(percent));
}

bool Speaker::wait_idle(int timeout_ms) {
    std::unique_lock<std::mutex> lk(m_);
    return cv_.wait_for(lk, std::chrono::milliseconds(timeout_ms),
                        [&] { return queue_.empty() && !playing_; });
}

SpeakerStats Speaker::stats() const {
    std::lock_guard<std::mutex> lk(m_);
    SpeakerStats s;
    s.enabled = enabled_;
    s.open = open_;
    s.playing = playing_;
    s.queued_ms = queued_rate_ > 0 ? (int)(queued_samples_ * 1000 / (size_t)queued_rate_) : 0;
    s.clips = clips_;
    s.dropped = dropped_;
    s.last_error = last_error_;
    return s;
}

void Speaker::shutdown() {
    {
        std::lock_guard<std::mutex> lk(m_);
        quit_ = true;
        cut_ = true;
        queue_.clear();
        queued_samples_ = 0;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

void Speaker::loop() {
    std::unique_ptr<IAudioOut> dev;
    int dev_rate = 0;
    auto close_locked = [&](std::unique_lock<std::mutex>& lk) {
        dev_ = nullptr;
        open_ = false;
        lk.unlock();
        dev.reset();                      // IMP_AO_DisableChn/Disable, outside the lock
        lk.lock();
        LOGI(MOD, "speaker closed");
    };

    std::unique_lock<std::mutex> lk(m_);
    while (!quit_) {
        if (queue_.empty()) {
            if (dev) {
                // Keep the output up for grace_ms: a page that plays a second
                // clip does not pay for a second open.
                const bool more = cv_.wait_for(lk, std::chrono::milliseconds(grace_ms_),
                                               [&] { return quit_ || !queue_.empty(); });
                if (!more && dev) close_locked(lk);
            } else {
                cv_.wait(lk, [&] { return quit_ || !queue_.empty(); });
            }
            continue;
        }
        Clip c = std::move(queue_.front());
        queue_.pop_front();
        queued_samples_ -= std::min(queued_samples_, c.pcm.size());

        if (dev && dev_rate != c.rate) close_locked(lk);
        if (!dev) {
            AudioParams p;
            p.sample_rate = c.rate;
            p.frame_ms = 40;
            p.volume = output_volume_to_vendor(volume_);
            lk.unlock();
            std::unique_ptr<IAudioOut> d = factory_(p);
            lk.lock();
            if (!d) {
                last_error_ = "the codec refused the speaker at " + std::to_string(c.rate) + " Hz";
                LOGW(MOD, "%s - clip dropped", last_error_.c_str());
                ++dropped_;
                cv_.notify_all();
                continue;
            }
            dev = std::move(d);
            dev_rate = c.rate;
            dev_ = dev.get();
            open_ = true;
            last_error_.clear();
            LOGI(MOD, "speaker opened (%d Hz)", c.rate);
        }

        playing_ = true;
        cut_ = false;
        lk.unlock();
        // 200 ms chunks: switching the speaker off cuts a long clip promptly
        // instead of after it has played to the end.
        const size_t chunk = (size_t)c.rate / 5;
        bool ok = true, cut = false;
        for (size_t off = 0; off < c.pcm.size(); off += chunk) {
            {
                std::lock_guard<std::mutex> g(m_);
                if (cut_ || quit_) { cut = true; break; }
            }
            const size_t n = std::min(chunk, c.pcm.size() - off);
            if (!dev->write(c.pcm.data() + off, n)) { ok = false; break; }
        }
        bool last = false;
        { std::lock_guard<std::mutex> g(m_); last = queue_.empty(); }
        // SendFrame only waits for ring space; the tail of the clip is still
        // in the device. Flush it when nothing follows, or it is cut off by
        // the close below.
        if (ok && !cut && last) dev->drain(0);
        lk.lock();
        playing_ = false;
        if (!ok) {
            last_error_ = "writing to the speaker failed";
            LOGW(MOD, "%s - output closed", last_error_.c_str());
            ++dropped_;
            close_locked(lk);
        } else if (cut) {
            ++dropped_;
        } else {
            ++clips_;
        }
        cv_.notify_all();
    }
    if (dev) close_locked(lk);
}

}} // namespace machino::audio
