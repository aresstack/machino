#include "core/pipeline.hpp"
#include "core/log.hpp"

namespace machino {

static const char* MOD = "PIPELINE";

Pipeline::Pipeline(IPlatform& platform, const EffectiveStream& stream, const PipelineConfig& cfg, StreamHub& hub)
    : platform_(platform), stream_(stream), cfg_(cfg), hub_(hub), pool_(8, 256 * 1024) {}

Pipeline::~Pipeline() { stop_pipeline(); }

Result Pipeline::acquire() {
    std::lock_guard<std::mutex> lk(m_);
    idle_since_ms_ = -1;
    if (!running_) { Result r = start_locked(); if (!r) return r; }
    ++refs_;
    LOGD(MOD, "acquire -> consumers=%d", refs_);
    return Result::ok();
}

void Pipeline::release() {
    std::lock_guard<std::mutex> lk(m_);
    if (refs_ > 0) --refs_;
    LOGD(MOD, "release -> consumers=%d", refs_);
    if (refs_ == 0 && !manual_ && running_) idle_since_ms_ = 0;
}

Result Pipeline::start_pipeline() {
    std::lock_guard<std::mutex> lk(m_);
    idle_since_ms_ = -1;
    if (!running_) { Result r = start_locked(); if (!r) return r; }
    manual_ = true;
    LOGI(MOD, "start_pipeline (consumers=%d)", refs_);
    return Result::ok();
}

void Pipeline::stop_pipeline() {
    std::lock_guard<std::mutex> lk(m_);
    manual_ = false;
    if (running_) { LOGI(MOD, "stop_pipeline (consumers=%d)", refs_); stop_locked(); }
    idle_since_ms_ = -1;
}

void Pipeline::tick(int64_t now_ms) {
    std::lock_guard<std::mutex> lk(m_);
    if (!running_ || refs_ > 0 || manual_) return;
    if (idle_since_ms_ == 0) { idle_since_ms_ = now_ms; return; }
    if (idle_since_ms_ > 0 && now_ms - idle_since_ms_ >= cfg_.grace_ms) {
        LOGI(MOD, "no consumer for %d ms - tearing down", cfg_.grace_ms);
        stop_locked();
        idle_since_ms_ = -1;
    }
}

Result Pipeline::start_locked() {
    LOGI(MOD, "bring-up on %s", platform_.name());
    Result r = platform_.bring_up();
    if (!r) { LOGE(MOD, "platform bring-up failed (%s, %d)", status_name(r.status), r.code); return r; }

    fs_ = platform_.create_framesource(0, stream_);
    if (!fs_) { LOGE(MOD, "framesource create failed"); platform_.tear_down(); return Result::error(); }

    enc_ = platform_.create_encoder(0, stream_);
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
    LOGI(MOD, "running (%dx%d@%d, %d kbps)", stream_.width, stream_.height, stream_.fps, stream_.bitrate_kbps);
    return Result::ok();
}

void Pipeline::stop_locked() {
    quit_ = true;
    if (thread_.joinable()) thread_.join();
    running_ = false;
    if (enc_) enc_->stop();
    if (fs_)  fs_->disable();
    if (bound_ && fs_ && enc_) platform_.unbind(*fs_, *enc_);
    bound_ = false;
    enc_.reset();
    fs_.reset();
    platform_.tear_down();
    LOGI(MOD, "stopped (%u frames this run, pool exhausted %u)", frames_.load(), pool_.exhausted());
    frames_ = 0;
}

void Pipeline::capture_loop() {
    LOGD(MOD, "capture thread up");
    unsigned timeouts = 0, dropped = 0;
    while (!quit_) {
        auto au = pool_.acquire();
        if (!au) {
            AccessUnit scratch; Result r = enc_->fetch(scratch, cfg_.poll_timeout_ms);
            if (r) { if ((++dropped % 50) == 1) LOGW(MOD, "pool exhausted - dropped %u frames", dropped); }
            continue;
        }
        Result r = enc_->fetch(*au, cfg_.poll_timeout_ms);
        if (r.status == Status::Timeout) {
            if ((++timeouts % 20) == 1) LOGW(MOD, "encoder idle (no frame for %u polls)", timeouts);
            continue;
        }
        if (!r) { LOGW(MOD, "fetch failed (%d)", r.code); continue; }
        timeouts = 0;
        au->seq = seq_++;
        unsigned n = ++frames_;
        if (n == 1) LOGI(MOD, "first frame: %lu bytes key=%d pts=%lld", (unsigned long)au->data.size(), (int)au->key, (long long)au->pts_us);
        hub_.publish(au);
    }
    LOGD(MOD, "capture thread down");
}

} // namespace machino
