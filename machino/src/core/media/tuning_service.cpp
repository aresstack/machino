#include "core/media/tuning_service.hpp"
#include "core/log.hpp"
#include <algorithm>

namespace machino { namespace media {

static const char* MOD = "tuning";
using power::ApplyResult;

TuningService::TuningService(lifecycle::PipelineManager& pipeline, IPlatform& platform, StreamHub& hub,
                             const EffectiveStream& base, const ImageSettings& image, const LatencySettings& latency)
    : pipeline_(pipeline), platform_(platform), hub_(hub), image_(platform.image()), base_(base), latency_(latency) {
    requested_.fill(-1); effective_.fill(-1);
    if (image_) image_caps_ = image_->caps();
    load_image_settings(image);
    EffectiveStream current = pipeline_.stream();
    {
        std::lock_guard<std::mutex> lk(m_);
        resolved_ = resolve_locked(current);
    }
    hub_.set_default_depth((size_t)resolved_.consumer_queue_depth);
    EffectiveStream s = pipeline_.stream();
    s.gop = resolved_.gop; s.buffers = resolved_.framesource_buffers; s.encoder_buffers = resolved_.encoder_buffers;
    std::string err; pipeline_.update_stream(s, false, err);       // cold setup; never creates demand
    pipeline_.set_post_start_hook([this] { apply_images_after_start(); });
    LOGI(MOD, "latency profile=%s gop=%d fs-buffers=%d enc-buffers=%d queue=%d",
         latency_profile_name(resolved_.profile), resolved_.gop, resolved_.framesource_buffers,
         resolved_.encoder_buffers, resolved_.consumer_queue_depth);
}

TuningService::~TuningService() { pipeline_.set_post_start_hook({}); }

void TuningService::load_image_settings(const ImageSettings& i) {
    auto put = [&](ImageControl c, const std::optional<int>& v) { if (v) requested_[(int)c] = *v; };
    put(ImageControl::Brightness, i.brightness); put(ImageControl::Contrast, i.contrast);
    put(ImageControl::Saturation, i.saturation); put(ImageControl::Sharpness, i.sharpness); put(ImageControl::Hue, i.hue);
    put(ImageControl::HFlip, i.hflip); put(ImageControl::VFlip, i.vflip); put(ImageControl::AntiFlicker, i.anti_flicker);
    put(ImageControl::AeCompensation, i.ae_compensation); put(ImageControl::HighlightDepress, i.highlight_depress);
    put(ImageControl::BacklightComp, i.backlight_comp); put(ImageControl::WhiteBalanceMode, i.white_balance_mode);
    put(ImageControl::RunningMode, i.running_mode); put(ImageControl::TemporalNr, i.temporal_nr);
    put(ImageControl::SpatialNr, i.spatial_nr); put(ImageControl::Dpc, i.dpc); put(ImageControl::Defog, i.defog);
}

ResolvedLatency TuningService::resolve_locked(const EffectiveStream& stream) const {
    ResolvedLatency r; r.profile = latency_.profile;
    r.gop = base_.gop; r.framesource_buffers = base_.buffers; r.encoder_buffers = base_.encoder_buffers; r.consumer_queue_depth = 4;
    if (latency_.profile == LatencyProfile::Low) {
        // One second between scheduled IDRs, one hardware buffer at each
        // explicit stage, and one AU per consumer. No undocumented vendor
        // "low latency" flag exists in SDK 1.3.1.
        r.gop = std::max(1, stream.fps);
        r.framesource_buffers = 1;
        r.encoder_buffers = 1;
        r.consumer_queue_depth = 1;
    }
    if (latency_.gop) r.gop = *latency_.gop;
    if (latency_.framesource_buffers) r.framesource_buffers = *latency_.framesource_buffers;
    if (latency_.encoder_buffers) r.encoder_buffers = *latency_.encoder_buffers;
    if (latency_.consumer_queue_depth) r.consumer_queue_depth = *latency_.consumer_queue_depth;
    return r;
}

ImageCaps TuningService::image_caps() const { return image_caps_; }

TuningState TuningService::state() const {
    std::lock_guard<std::mutex> lk(m_);
    TuningState s; s.latency = resolved_; s.requested_latency = latency_;
    s.image_requested = requested_; s.image_effective = effective_;
    return s;
}

Result TuningService::exposure(ExposureReadback& out) {
    if (!image_) { out = ExposureReadback{}; return Result::unsupported(); }
    return pipeline_.read_exposure(out);
}

ApplyResult TuningService::set_image(ImageControl c, int value) { return apply_image(c, value, true); }
// See the header: the preview must not rewrite what the config reports.
ApplyResult TuningService::set_image_live(ImageControl c, int value) { return apply_image(c, value, false); }

ApplyResult TuningService::apply_image(ImageControl c, int value, bool record_requested) {
    const RangeCap cap = image_caps_.control[(int)c];
    if (!image_ || cap.support != Cap::Supported)
        return ApplyResult::rejected(ApplyMode::Unsupported, value, std::string(image_control_name(c)) + " is not supported");
    if (!cap.in_range(value) || (c == ImageControl::AntiFlicker && value != 0 && value != 50 && value != 60))
        return ApplyResult::rejected(cap.apply, value, std::string(image_control_name(c)) + " outside supported values");
    if (record_requested) {
        std::lock_guard<std::mutex> lk(m_);
        requested_[(int)c] = value;
    }
    int eff = -1; Result r = pipeline_.live_image(c, value, eff);
    if (r.status == Status::Busy)
        return ApplyResult::stored(cap.apply, value, "stored; pipeline transition in progress");
    if (!r) return ApplyResult::rejected(cap.apply, value, "ISP rejected the image control");
    {
        std::lock_guard<std::mutex> lk(m_);
        effective_[(int)c] = eff;
    }
    return ApplyResult::applied(cap.apply, value, eff >= 0 ? eff : value, "read back from ISP");
}

void TuningService::apply_images_after_start() {
    std::array<int, (int)ImageControl::COUNT> values;
    { std::lock_guard<std::mutex> lk(m_); values = requested_; }
    if (!image_) return;
    for (int i = 0; i < (int)ImageControl::COUNT; ++i) {
        if (values[i] < 0) continue;                    // preserve tuning-bin defaults
        ImageControl c = (ImageControl)i; int eff = -1;
        Result r = image_->set(c, values[i], eff);
        if (!r) { LOGW(MOD, "reapply %s=%d failed (%s,%d)", image_control_name(c), values[i], status_name(r.status), r.code); continue; }
        std::lock_guard<std::mutex> lk(m_); effective_[i] = eff;
    }
}

ApplyResult TuningService::clear_image(ImageControl c) {
    if (image_caps_.control[(int)c].support != Cap::Supported)
        return ApplyResult::rejected(ApplyMode::Unsupported, -1, std::string(image_control_name(c)) + " is not supported");
    {
        std::lock_guard<std::mutex> lk(m_);
        requested_[(int)c] = -1;
        effective_[(int)c] = -1;
    }
    // SDK 1.3.1 has no "restore IQ default" call, so a RUNNING pipeline is
    // restarted once: apply_images_after_start skips cleared controls and the
    // tuning-bin default becomes effective again. A cold pipeline simply
    // starts clean the next time.
    lifecycle::State st = pipeline_.state();
    if (st == lifecycle::State::Active || st == lifecycle::State::GraceIdle) {
        std::string err;
        Result r = pipeline_.update_stream(pipeline_.stream(), true, err);
        if (!r) return ApplyResult::rejected(ApplyMode::PipelineRestart, -1,
                                             std::string(image_control_name(c)) + ": restart to defaults failed: " + err);
        return ApplyResult::applied(ApplyMode::PipelineRestart, -1, -1, "cleared; pipeline restarted to tuning defaults");
    }
    return ApplyResult::applied(ApplyMode::Live, -1, -1, "cleared");
}

ApplyResult TuningService::clear_framesource_buffers() {
    EffectiveStream current = pipeline_.stream(); ResolvedLatency target;
    { std::lock_guard<std::mutex> lk(m_); latency_.framesource_buffers.reset(); target = resolve_locked(current); }
    return apply_latency(target, 0, "FrameSource buffers (reset)");
}

ApplyResult TuningService::clear_encoder_buffers() {
    EffectiveStream current = pipeline_.stream(); ResolvedLatency target;
    { std::lock_guard<std::mutex> lk(m_); latency_.encoder_buffers.reset(); target = resolve_locked(current); }
    return apply_latency(target, 0, "encoder buffers (reset)");
}

ApplyResult TuningService::clear_queue_depth() {
    EffectiveStream current = pipeline_.stream(); ResolvedLatency target;
    { std::lock_guard<std::mutex> lk(m_); latency_.consumer_queue_depth.reset(); target = resolve_locked(current); }
    return apply_latency(target, 0, "consumer queue depth (reset)");
}

ApplyResult TuningService::apply_latency(const ResolvedLatency& target, int requested, const char* what) {
    EffectiveStream s = pipeline_.stream();
    const bool restart_change = s.buffers != target.framesource_buffers || s.encoder_buffers != target.encoder_buffers;
    const bool gop_change = s.gop != target.gop;
    s.gop = target.gop; s.buffers = target.framesource_buffers; s.encoder_buffers = target.encoder_buffers;
    hub_.set_default_depth((size_t)target.consumer_queue_depth);
    std::string err;
    lifecycle::State state = pipeline_.state();
    const bool running = state == lifecycle::State::Active || state == lifecycle::State::GraceIdle;
    if (restart_change) {
        Result r = pipeline_.update_stream(s, running, err);
        if (!r) return ApplyResult::rejected(ApplyMode::PipelineRestart, requested, std::string(what) + ": restart failed: " + err);
    } else if (gop_change && running) {
        int eff = -1; Result r = pipeline_.live_gop(target.gop, eff);
        if (!r) return ApplyResult::rejected(ApplyMode::Live, requested, std::string(what) + ": encoder rejected GOP");
        s.gop = eff > 0 ? eff : target.gop;   // accepted value, not a pending read-back
    } else if (gop_change) {
        Result r = pipeline_.update_stream(s, false, err);
        if (!r) return ApplyResult::rejected(ApplyMode::PipelineRestart, requested, std::string(what) + ": could not store stream settings");
    }
    {
        std::lock_guard<std::mutex> lk(m_);
        resolved_ = target;
        if (running && gop_change && !restart_change) resolved_.gop = s.gop;
    }
    ApplyMode mode = restart_change ? ApplyMode::PipelineRestart : ApplyMode::Live;
    if (!running && (restart_change || gop_change)) return ApplyResult::stored(mode, requested);
    return ApplyResult::applied(mode, requested, requested, restart_change ? "pipeline restarted once" : "applied without restart");
}

ApplyResult TuningService::set_latency_profile(LatencyProfile p) {
    EffectiveStream current = pipeline_.stream(); ResolvedLatency target;
    { std::lock_guard<std::mutex> lk(m_); latency_.profile = p; target = resolve_locked(current); }
    return apply_latency(target, (int)p, "latency profile");
}

ApplyResult TuningService::set_gop(int frames) {
    if (frames < 1 || frames > 1000) return ApplyResult::rejected(ApplyMode::Live, frames, "GOP must be in 1..1000");
    EffectiveStream current = pipeline_.stream(); ResolvedLatency target;
    { std::lock_guard<std::mutex> lk(m_); latency_.gop = frames; target = resolve_locked(current); }
    return apply_latency(target, frames, "GOP");
}

ApplyResult TuningService::set_framesource_buffers(int n) {
    if (n < 1 || n > 8) return ApplyResult::rejected(ApplyMode::PipelineRestart, n, "FrameSource buffers must be in 1..8");
    EffectiveStream current = pipeline_.stream(); ResolvedLatency target;
    { std::lock_guard<std::mutex> lk(m_); latency_.framesource_buffers = n; target = resolve_locked(current); }
    return apply_latency(target, n, "FrameSource buffers");
}

ApplyResult TuningService::set_encoder_buffers(int n) {
    if (n < 1 || n > 8) return ApplyResult::rejected(ApplyMode::PipelineRestart, n, "encoder buffers must be in 1..8");
    EffectiveStream current = pipeline_.stream(); ResolvedLatency target;
    { std::lock_guard<std::mutex> lk(m_); latency_.encoder_buffers = n; target = resolve_locked(current); }
    return apply_latency(target, n, "encoder buffers");
}

ApplyResult TuningService::set_queue_depth(int n) {
    if (n < 1 || n > 32) return ApplyResult::rejected(ApplyMode::Live, n, "consumer queue depth must be in 1..32");
    EffectiveStream current = pipeline_.stream(); ResolvedLatency target;
    { std::lock_guard<std::mutex> lk(m_); latency_.consumer_queue_depth = n; target = resolve_locked(current); }
    return apply_latency(target, n, "consumer queue depth");
}

ApplyResult TuningService::refresh_after_stream_change() {
    EffectiveStream current = pipeline_.stream(); ResolvedLatency target;
    { std::lock_guard<std::mutex> lk(m_); target = resolve_locked(current); }
    return apply_latency(target, (int)target.profile, "latency profile refresh");
}

}} // namespace machino::media
