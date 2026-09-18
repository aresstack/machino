#include "core/pipeline.hpp"
#include "core/log.hpp"

namespace machino {

static const char* MOD = "PIPELINE";

Pipeline::Pipeline(IPlatform& platform, const AppConfig& cfg, StreamHub& hub)
    : platform_(platform), cfg_(cfg), hub_(hub) {}

Pipeline::~Pipeline() { stop(); }

Result Pipeline::acquire() {
    std::lock_guard<std::mutex> lk(m_);
    idle_since_ms_ = -1;
    if (!running_) {
        Result r = start_locked();
        if (!r) return r;
    }
    ++refs_;
    LOGD(MOD, "acquire -> consumers=%d", refs_);
    return Result::ok();
}

void Pipeline::release() {
    std::lock_guard<std::mutex> lk(m_);
    if (refs_ > 0) --refs_;
    LOGD(MOD, "release -> consumers=%d", refs_);
    if (refs_ == 0 && running_) idle_since_ms_ = 0;   // armed; tick() stamps the time
}

void Pipeline::tick(int64_t now_ms) {
    std::lock_guard<std::mutex> lk(m_);
    if (!running_ || refs_ > 0) return;
    if (idle_since_ms_ == 0) { idle_since_ms_ = now_ms; return; }
    if (idle_since_ms_ > 0 && now_ms - idle_since_ms_ >= cfg_.pipeline.grace_ms) {
        LOGI(MOD, "no consumer for %d ms - tearing down", cfg_.pipeline.grace_ms);
        stop_locked();
        idle_since_ms_ = -1;
    }
}

void Pipeline::stop() {
    std::lock_guard<std::mutex> lk(m_);
    if (running_) stop_locked();
    refs_ = 0;
}

Result Pipeline::start_locked() {
    LOGI(MOD, "bring-up on %s", platform_.name());
    Result r = platform_.bring_up();
    if (!r) { LOGE(MOD, "platform bring-up failed (%s, %d)", status_name(r.status), r.code); return r; }

    fs_ = platform_.create_framesource(0, cfg_.video);
    if (!fs_) { LOGE(MOD, "framesource create failed"); platform_.tear_down(); return Result::error(); }

    enc_ = platform_.create_encoder(0, cfg_.video);
    if (!enc_) { LOGE(MOD, "encoder create failed"); fs_.reset(); platform_.tear_down(); return Result::error(); }

    r = platform_.bind(*fs_, *enc_);
    if (!r) { LOGE(MOD, "bind failed (%d)", r.code); enc_.reset(); fs_.reset(); platform_.tear_down(); return r; }
    bound_ = true;

    r = fs_->enable();
    if (!r) { LOGE(MOD, "framesource enable failed (%d)", r.code); stop_locked(); return r; }

    r = enc_->start();
    if (!r) { LOGE(MOD, "encoder start failed (%d)", r.code); stop_locked(); return r; }

    quit_ = false;
    running_ = true;
    thread_ = std::thread([this] { capture_loop(); });
    LOGI(MOD, "running (%dx%d@%d, %d kbps)", cfg_.video.width, cfg_.video.height,
         cfg_.video.fps, cfg_.video.bitrate_kbps);
    return Result::ok();
}

// Reverse order of start_locked(). Safe to call from a partially started state.
void Pipeline::stop_locked() {
    quit_ = true;
    if (thread_.joinable()) thread_.join();
    running_ = false;
    if (enc_) enc_->stop();
    if (fs_)  fs_->disable();
    if (bound_ && fs_ && enc_) platform_.unbind(*fs_, *enc_);
    bound_ = false;
    enc_.reset();       // destroys encoder channel/group
    fs_.reset();        // destroys frame-source channel
    platform_.tear_down();
    LOGI(MOD, "stopped");
}

void Pipeline::capture_loop() {
    LOGD(MOD, "capture thread up");
    unsigned timeouts = 0;
    while (!quit_) {
        auto au = std::make_shared<AccessUnit>();
        Result r = enc_->fetch(*au, cfg_.pipeline.poll_timeout_ms);
        if (r.status == Status::Timeout) {
            if ((++timeouts % 20) == 1) LOGW(MOD, "encoder idle (no frame for %u polls)", timeouts);
            continue;
        }
        if (!r) { LOGW(MOD, "fetch failed (%d)", r.code); continue; }
        timeouts = 0;
        au->seq = seq_++;
        if (au->seq == 0)
            LOGI(MOD, "first frame: %zu bytes key=%d pts=%lld", au->data.size(), (int)au->key, (long long)au->pts_us);
        hub_.publish(au);
    }
    LOGD(MOD, "capture thread down");
}

} // namespace machino
