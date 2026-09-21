#include "core/power/performance_service.hpp"
#include "core/log.hpp"
#include <algorithm>

namespace machino { namespace power {

static const char* MOD = "PERF";

PerformanceService::PerformanceService(lifecycle::PipelineManager& pipeline, IPlatform& platform, ISystemStats& stats,
                                       const hw::ResolvedHardware& hw, const StreamConfig& video)
    : pipeline_(pipeline), platform_(platform), stats_(stats), hw_(hw) {
    caps_ = platform_.capabilities();
    if (IPowerControl* pc = platform_.power()) pc->fill_capabilities(caps_);
    // sensor fps bounds: only from verified modes (never invented)
    std::vector<int> v = verified_fps();
    if (v.size() >= 2) { caps_.sensor.fps.min = v.front(); caps_.sensor.fps.max = v.back();
                         caps_.video.fps.min = v.front();  caps_.video.fps.max = v.back(); }
    if (caps_.video.fps.support == Cap::Unknown) caps_.video.fps.support = Cap::Supported;   // FrameSource rate (restart) always exists
    if (caps_.video.fps.apply == ApplyMode::Unsupported) caps_.video.fps.apply = ApplyMode::PipelineRestart;
    sensor_fps_req_ = hw_.mode.value.fps;
    bitrate_default_ = video.bitrate_kbps;
    allow_unverified_ = hw_.allow_unverified_mode;
}

CapabilitySet PerformanceService::capabilities() const { return caps_; }

std::vector<int> PerformanceService::verified_fps() const {
    std::vector<int> v;
    for (const auto& m : hw_.sensor.modes)
        if (m.width == hw_.mode.value.width && m.height == hw_.mode.value.height) v.push_back(m.fps);
    std::sort(v.begin(), v.end()); v.erase(std::unique(v.begin(), v.end()), v.end());
    return v;
}

ApplyMode PerformanceService::sensor_fps_mode() const {
    return caps_.sensor.fps.support == Cap::Supported ? caps_.sensor.fps.apply : ApplyMode::Unsupported;
}

EffectiveState PerformanceService::effective_state() {
    std::lock_guard<std::mutex> lk(m_);
    EffectiveState e;
    EffectiveStream s = pipeline_.stream();
    e.profile = profile_;
    e.sensor_fps_requested = sensor_fps_req_;
    int rb = -1;
    if (pipeline_.read_sensor_fps(rb) && rb > 0) { e.sensor_fps_effective = rb; e.sensor_fps_readback = true; }
    e.stream_fps = s.fps; e.bitrate_kbps = s.bitrate_kbps;
    e.isp = isp_; e.encoder = enc_; e.cpu = cpu_;
    e.sensor_fps_mode = sensor_fps_mode();
    e.stream_fps_mode = caps_.video.fps.apply;
    e.bitrate_mode = caps_.video.bitrate.support == Cap::Supported ? caps_.video.bitrate.apply : ApplyMode::Unsupported;
    return e;
}

Telemetry PerformanceService::telemetry() {
    Telemetry t;
    lifecycle::Stats st = pipeline_.stats();
    lifecycle::Measurement m = pipeline_.measurement();
    EffectiveState e = effective_state();
    t.state = st.state; t.generation = st.generation; t.profile = e.profile;
    t.requested_sensor_fps = e.sensor_fps_requested;
    if (e.sensor_fps_readback) t.effective_sensor_fps = Optional<int>::of(e.sensor_fps_effective);
    t.sensor_fps_readback = e.sensor_fps_readback;
    t.requested_stream_fps = e.stream_fps;
    t.requested_bitrate_kbps = e.bitrate_kbps;
    if (m.valid) { t.measured_encoded_fps = Optional<double>::of(m.encoded_fps); t.measured_bitrate_kbps = Optional<double>::of(m.bitrate_kbps); }
    t.dropped_frames = m.dropped_frames;
    ProcessStats ps = stats_.sample();
    if (ps.available) { t.rss_kb = Optional<uint64_t>::of(ps.rss_kb); t.threads = Optional<int>::of(ps.threads);
                        if (ps.cpu_percent >= 0) t.cpu_percent = Optional<double>::of(ps.cpu_percent); }
    if (IPowerControl* pc = platform_.power()) {
        PowerState p = pc->current_state();
        if (p.isp_clock.available)     t.isp_clock_hz     = Optional<uint64_t>::of(p.isp_clock.hz);
        if (p.encoder_clock.available) t.encoder_clock_hz = Optional<uint64_t>::of(p.encoder_clock.hz);
        if (p.cpu_clock.available)     t.cpu_freq_khz     = Optional<uint64_t>::of(p.cpu_clock.hz / 1000);
    }
    return t;
}

// ---- presets ---------------------------------------------------------------
int PerformanceService::preset_fps(Profile p, std::string& note) const {
    std::vector<int> v = verified_fps();
    const hw::BoardPresets& bp = hw_.presets;
    int def = hw_.mode.value.fps;
    switch (p) {
        case Profile::Performance: return def;
        case Profile::Balanced:
            if (bp.balanced_fps) return *bp.balanced_fps;
            if (v.size() >= 2) { for (size_t i = v.size(); i-- > 0;) if (v[i] < def) return v[i]; }
            note = "no lower verified operating point - balanced == performance"; return def;
        case Profile::Battery:
            if (bp.battery_fps) return *bp.battery_fps;
            if (!v.empty() && v.front() < def) return v.front();
            note = "no lower verified operating point - battery == performance"; return def;
        case Profile::Custom: return def;
    }
    return def;
}

int PerformanceService::preset_bitrate(Profile p) const {
    const hw::BoardPresets& bp = hw_.presets;
    if (p == Profile::Balanced && bp.balanced_bitrate) return *bp.balanced_bitrate;
    if (p == Profile::Battery  && bp.battery_bitrate)  return *bp.battery_bitrate;
    return bitrate_default_;
}

std::string PerformanceService::check_stream_fps(int fps) const {
    if (fps <= 0) return "fps must be > 0";
    if (!caps_.video.fps.in_range(fps)) return "fps outside known range";
    std::vector<int> v = verified_fps();
    if (!v.empty() && std::find(v.begin(), v.end(), fps) == v.end() && !allow_unverified_)
        return "not a verified operating point (set sensor.allow_unverified_mode = 1 to test)";
    return "";
}

std::string PerformanceService::check_bitrate(int kbps) const {
    if (kbps <= 0) return "bitrate must be > 0";
    if (!caps_.video.bitrate.in_range(kbps)) return "bitrate outside known range";
    return "";
}

std::string PerformanceService::check_sensor_fps(int fps) const {
    if (fps <= 0) return "fps must be > 0";
    if (!caps_.sensor.fps.in_range(fps)) return "sensor fps outside known range";
    return "";
}

ApplyResult PerformanceService::apply_profile(Profile p) {
    if (p == Profile::Custom) { std::lock_guard<std::mutex> lk(m_); profile_ = p; return ApplyResult::applied(ApplyMode::Live, (int)p, (int)p, "custom: individual settings apply"); }
    std::string note; int fps = preset_fps(p, note); int kbps = preset_bitrate(p);
    // A profile is one operating point, not three settings that happen to be
    // applied in a row. Check every component before touching anything:
    // applying the fps and then rejecting the bitrate would leave the camera at
    // a point nobody asked for while the caller is told the profile was refused.
    auto refuse = [&](ApplyMode m, int req, const char* what, const std::string& reason) {
        return ApplyResult::rejected(m, req, std::string(profile_name(p)) + " not applied (nothing changed): " +
                                     what + " " + std::to_string(req) + ": " + reason);
    };
    std::string why = check_stream_fps(fps);
    if (!why.empty()) return refuse(caps_.video.fps.apply, fps, "stream fps", why);
    why = check_bitrate(kbps);
    if (!why.empty()) return refuse(caps_.video.bitrate.apply, kbps, "bitrate", why);
    // Where the platform has a sensor rate control, that rate is part of the
    // operating point and its range belongs in this check. Where it has none,
    // the stream rate IS the operating point: the profile is then complete
    // without it, and reporting that as a failure would make every profile
    // change fail on such a platform.
    const bool sensor_rate_applies = sensor_fps_mode() != ApplyMode::Unsupported;
    if (sensor_rate_applies) {
        why = check_sensor_fps(fps);
        if (!why.empty()) return refuse(sensor_fps_mode(), fps, "sensor fps", why);
    }
    LOGI(MOD, "profile %s -> fps=%d bitrate=%d%s%s", profile_name(p), fps, kbps, note.empty() ? "" : " (", note.empty() ? "" : (note + ")").c_str());

    ApplyResult r1 = set_stream_fps(fps);        // FrameSource + encoder rate (restart if running)
    if (!r1.ok) return r1;                       // nothing else has been touched yet
    // From here parts of the profile are in effect. Validation cannot rule out
    // a hardware refusal, and there is no trustworthy way back: restoring the
    // old value would go through the very setter that just failed. So no
    // rollback is faked. The box is then at a mixed operating point - which is
    // what "custom" means - and the caller is told which part did not take,
    // instead of being handed a profile name that does not describe the box.
    auto partial = [&](const ApplyResult& f, const char* which) {
        { std::lock_guard<std::mutex> lk(m_); profile_ = Profile::Custom; }
        LOGW(MOD, "profile %s only partially applied: %s failed (%s)", profile_name(p), which, f.message.c_str());
        return ApplyResult::rejected(f.mode, f.requested,
            std::string(profile_name(p)) + " only partially applied: " + which + " failed (" + f.message +
            "); stream fps " + std::to_string(fps) + " is in effect, no rollback was attempted - reported as custom");
    };
    ApplyResult r2 = sensor_rate_applies
        ? set_sensor_fps(fps)
        : ApplyResult::applied(ApplyMode::Unsupported, fps, -1, "platform has no sensor rate control");
    if (!r2.ok) return partial(r2, "sensor fps");
    ApplyResult r3 = set_bitrate(kbps);
    if (!r3.ok) return partial(r3, "bitrate");
    { std::lock_guard<std::mutex> lk(m_); profile_ = p; }
    ApplyResult r = ApplyResult::applied(r1.mode, fps, r1.effective, note.c_str());
    r.deferred = r1.deferred;
    return r;
}

// ---- setters -----------------------------------------------------------------
ApplyResult PerformanceService::apply_stream_restart(const EffectiveStream& s, const char* what, int requested) {
    std::string err;
    bool running = pipeline_.state() == lifecycle::State::Active || pipeline_.state() == lifecycle::State::GraceIdle;
    Result r = pipeline_.update_stream(s, true, err);
    if (!r) return ApplyResult::rejected(ApplyMode::PipelineRestart, requested, std::string(what) + ": restart failed: " + err);
    if (!running) return ApplyResult::stored(ApplyMode::PipelineRestart, requested);
    return ApplyResult::applied(ApplyMode::PipelineRestart, requested, requested, "pipeline restarted with demand preserved");
}

ApplyResult PerformanceService::set_stream_fps(int fps) {
    std::string why = check_stream_fps(fps);
    if (!why.empty()) return ApplyResult::rejected(caps_.video.fps.apply, fps, why);
    std::vector<int> v = verified_fps();
    if (!v.empty() && std::find(v.begin(), v.end(), fps) == v.end())
        LOGW(MOD, "stream fps %d is not a verified operating point (allowed by config)", fps);
    EffectiveStream s = pipeline_.stream();
    if (s.fps == fps) return ApplyResult::applied(ApplyMode::Live, fps, fps, "unchanged");
    s.fps = fps;
    { std::lock_guard<std::mutex> lk(m_); profile_ = Profile::Custom; }
    return apply_stream_restart(s, "stream fps", fps);
}

ApplyResult PerformanceService::set_sensor_fps(int fps) {
    ApplyMode mode = sensor_fps_mode();
    if (fps <= 0) return ApplyResult::rejected(mode, fps, "fps must be > 0");
    if (mode == ApplyMode::Unsupported) { std::lock_guard<std::mutex> lk(m_); sensor_fps_req_ = fps; return ApplyResult::rejected(mode, fps, "sensor fps control unsupported on this platform (stream fps only)"); }
    std::string why = check_sensor_fps(fps);
    if (!why.empty()) return ApplyResult::rejected(mode, fps, why);
    { std::lock_guard<std::mutex> lk(m_); sensor_fps_req_ = fps; }
    pipeline_.set_sensor_fps_target(fps);
    int eff = -1;
    Result r = pipeline_.live_sensor_fps(fps, eff);
    if (r.status == Status::Busy) return ApplyResult::stored(mode, fps, "stored; applied at next pipeline start");
    if (!r) return ApplyResult::rejected(mode, fps, "platform rejected sensor fps (" + std::string(status_name(r.status)) + ")");
    return ApplyResult::applied(mode, fps, eff > 0 ? eff : fps, eff > 0 ? "read back from hardware" : "no readback available");
}

ApplyResult PerformanceService::clear_stream_fps() {
    const int def = hw_.mode.value.fps;             // cold-start default: the mode's own rate
    EffectiveStream s = pipeline_.stream();
    if (s.fps == def) return ApplyResult::applied(ApplyMode::Live, def, def, "already at the mode default");
    s.fps = def;
    return apply_stream_restart(s, "stream fps (reset)", def);
}

ApplyResult PerformanceService::clear_sensor_fps() {
    const int def = hw_.mode.value.fps;
    { std::lock_guard<std::mutex> lk(m_); sensor_fps_req_ = def; }
    ApplyMode mode = sensor_fps_mode();
    if (mode == ApplyMode::Unsupported)
        return ApplyResult::applied(ApplyMode::Live, def, def, "sensor fps follows the mode on this platform");
    pipeline_.set_sensor_fps_target(def);
    int eff = -1;
    Result r = pipeline_.live_sensor_fps(def, eff);
    if (r.status == Status::Busy) return ApplyResult::stored(mode, def, "stored; applied at next pipeline start");
    if (!r) return ApplyResult::rejected(mode, def, "platform rejected sensor fps (" + std::string(status_name(r.status)) + ")");
    return ApplyResult::applied(mode, def, eff > 0 ? eff : def, eff > 0 ? "read back from hardware" : "no readback available");
}

ApplyResult PerformanceService::set_bitrate(int kbps) {
    ApplyMode mode = caps_.video.bitrate.support == Cap::Supported ? caps_.video.bitrate.apply : ApplyMode::PipelineRestart;
    std::string why = check_bitrate(kbps);
    if (!why.empty()) return ApplyResult::rejected(mode, kbps, why);
    EffectiveStream s = pipeline_.stream();
    if (s.bitrate_kbps == kbps) return ApplyResult::applied(ApplyMode::Live, kbps, kbps, "unchanged");
    { std::lock_guard<std::mutex> lk(m_); profile_ = Profile::Custom; }
    if (mode == ApplyMode::Live) {
        int eff = -1; Result r = pipeline_.live_bitrate(kbps, eff);
        if (r) return ApplyResult::applied(ApplyMode::Live, kbps, eff > 0 ? eff : kbps, "read back from encoder");
        if (r.status != Status::Busy) LOGW(MOD, "live bitrate failed (%s) - falling back to restart", status_name(r.status));
    }
    s.bitrate_kbps = kbps;
    std::string err;
    Result r = pipeline_.update_stream(s, mode != ApplyMode::Live, err);   // cold: stored; live-capable: already handled
    if (!r) return ApplyResult::rejected(ApplyMode::PipelineRestart, kbps, "restart failed: " + err);
    return ApplyResult::stored(mode == ApplyMode::Live ? ApplyMode::Live : ApplyMode::PipelineRestart, kbps);
}

static ApplyResult perf_apply(IPlatform& platform, const char* what, PerfLevel l, ApplyResult (IPowerControl::*fn)(PerfLevel)) {
    IPowerControl* pc = platform.power();
    if (!pc) return ApplyResult::rejected(ApplyMode::Unsupported, (int)l, std::string(what) + ": no power control on this platform");
    return (pc->*fn)(l);
}

ApplyResult PerformanceService::set_isp_performance(PerfLevel l) {
    ApplyResult r = perf_apply(platform_, "isp performance", l, &IPowerControl::set_isp_performance);
    if (r.ok) { std::lock_guard<std::mutex> lk(m_); isp_ = l; }
    return r;
}
ApplyResult PerformanceService::set_encoder_performance(PerfLevel l) {
    ApplyResult r = perf_apply(platform_, "encoder performance", l, &IPowerControl::set_encoder_performance);
    if (r.ok) { std::lock_guard<std::mutex> lk(m_); enc_ = l; }
    return r;
}
ApplyResult PerformanceService::set_cpu_performance(PerfLevel l) {
    ApplyResult r = perf_apply(platform_, "cpu performance", l, &IPowerControl::set_cpu_performance);
    if (r.ok) { std::lock_guard<std::mutex> lk(m_); cpu_ = l; }
    return r;
}

// ---- config (re)load -----------------------------------------------------------
std::vector<ApplyResult> PerformanceService::apply_config(const PerformanceConfig& pc, const StreamConfig& video) {
    std::vector<ApplyResult> out;
    auto log = [&](const char* what, const ApplyResult& r) {
        if (r.ok) LOGI(MOD, "%s: requested=%d effective=%d mode=%s%s %s", what, r.requested, r.effective, apply_mode_name(r.mode), r.deferred ? " (deferred)" : "", r.message.c_str());
        else LOGW(MOD, "%s: REJECTED requested=%d mode=%s: %s", what, r.requested, apply_mode_name(r.mode), r.message.c_str());
        out.push_back(r);
    };
    if (pc.profile != Profile::Custom) { log("profile", apply_profile(pc.profile)); }
    else {
        if (video.fps)  log("video.fps", set_stream_fps(*video.fps));
        if (pc.sensor_fps > 0) log("sensor.fps", set_sensor_fps(pc.sensor_fps));
        log("video.bitrate", set_bitrate(video.bitrate_kbps));
    }
    // power levels: "auto" means adapter/kernel default - nothing is forced
    if (pc.isp != PerfLevel::Auto)     log("power.isp_performance",     set_isp_performance(pc.isp));
    if (pc.encoder != PerfLevel::Auto) log("power.encoder_performance", set_encoder_performance(pc.encoder));
    if (pc.cpu != PerfLevel::Auto)     log("power.cpu_performance",     set_cpu_performance(pc.cpu));
    return out;
}

}} // namespace machino::power
