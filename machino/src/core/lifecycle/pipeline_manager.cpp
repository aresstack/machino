#include "core/lifecycle/pipeline_manager.hpp"
#include "core/log.hpp"
#include <ctime>

namespace machino { namespace lifecycle {

static const char* MOD = "lifecycle";

static int64_t mono_us() { struct timespec ts{}; clock_gettime(CLOCK_MONOTONIC, &ts); return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000; }

const char* state_name(State s) {
    switch (s) {
        case State::ColdIdle:  return "COLD_IDLE";
        case State::Starting:  return "STARTING";
        case State::Active:    return "ACTIVE";
        case State::GraceIdle: return "GRACE_IDLE";
        case State::Stopping:  return "STOPPING";
        case State::Failed:    return "FAILED";
    }
    return "?";
}

void DemandHandle::release() {
    if (!mgr_) return;
    PipelineManager* m = mgr_; mgr_ = nullptr;
    m->release(type_, unit_);
}

PipelineManager::PipelineManager(IPlatform& platform, const EffectiveStream& stream, const LifecycleConfig& cfg,
                                 IGraceTimer& timer, StreamHub& hub)
    : platform_(platform), cfg_(cfg), timer_(timer) {
    // 16 slots > the sum of all consumer queue depths (StreamHub sinks hold at
    // most 4 each): a stalled consumer drops its own oldest frames, it can
    // never starve the capture path.
    units_[UNIT_MAIN].configured = true;
    units_[UNIT_MAIN].stream = stream;
    units_[UNIT_MAIN].hub = &hub;
    units_[UNIT_MAIN].pool = std::make_unique<AuPool>(16, 128 * 1024);
}

PipelineManager::~PipelineManager() { shutdown(); }

void PipelineManager::configure_sub(const EffectiveStream& s, StreamHub& hub, IGraceTimer* timer) {
    std::lock_guard<std::mutex> lk(m_);
    units_[UNIT_SUB].configured = true;
    units_[UNIT_SUB].stream = s;
    units_[UNIT_SUB].hub = &hub;
    units_[UNIT_SUB].timer = timer;
    if (!units_[UNIT_SUB].pool) units_[UNIT_SUB].pool = std::make_unique<AuPool>(16, 64 * 1024);
}

void PipelineManager::set_unit_grace_timer(int unit, IGraceTimer* timer) {
    std::lock_guard<std::mutex> lk(m_);
    if (unit >= 0 && unit <= UNIT_SUB) units_[unit].timer = timer;
}

void PipelineManager::configure_jpeg(const JpegParams& p, int cache_ms, int grace_ms, IGraceTimer* timer) {
    std::lock_guard<std::mutex> lk(m_);
    jpeg_.configured = true;
    jpeg_.params = p;
    jpeg_.cache_ms = cache_ms;
    jpeg_.grace_ms = grace_ms;
    jpeg_.timer = timer;
}

void PipelineManager::transition(State to, const char* why) {
    LOGI(MOD, "%s -> %s%s%s", state_name(state_), state_name(to), why ? " " : "", why ? why : "");
    State from = state_;
    state_ = to;
    if (listener_) listener_(from, to);
}

int PipelineManager::total_all_locked() const {
    return units_[UNIT_MAIN].total + units_[UNIT_SUB].total + jpeg_.demand + ai_demand_;
}

// ---- demand ---------------------------------------------------------------
DemandHandle PipelineManager::acquire(ConsumerType type, Result* result) {
    return acquire_unit(UNIT_MAIN, type, result);
}

bool PipelineManager::ensure_base_locked(ConsumerType type, int unit) {
    switch (state_) {
        case State::Failed:
            // STICKY. A failed bring-up is NOT retried - not by a new consumer,
            // not by a returning one, not by the same one reconnecting.
            //
            // Removing the five-retry loop inside SystemSession only capped the
            // burst; every consumer connect still called straight back in here,
            // which turned "5 retries per round" into "1 retry per connect" -
            // and a browser or RTSP client that reconnects is an unbounded
            // source of connects. Each failed vendor init costs about 1.2 MB
            // that is never returned, so on a 42 MB camera that is still a slow
            // OOM. See docs/incident-2026-09-22-oom.md.
            //
            // The daemon has no defined recovery event for a half-initialised
            // vendor stack, so the only way out is a restart of the daemon.
            // That is deliberate: a camera that answers 503 and says why beats
            // one that dies of an unbounded retry it never reported.
            if (!failed_refused_) {
                failed_refused_ = true;
                LOGE(MOD, "refusing bring-up: pipeline is FAILED (%s) - it stays FAILED until the daemon restarts", last_error_.c_str());
            }
            return false;
        case State::ColdIdle: {
            char why[64]; snprintf(why, sizeof why, "(consumer=%s unit=%s)", consumer_name(type), unit_name(unit));
            transition(State::Starting, why);
            if (!start_base_locked(unit)) {
                transition(State::Failed, last_error_.c_str());
                return false;
            }
            transition(State::Active, nullptr);
            return true;
        }
        case State::GraceIdle:
            timer_.disarm();
            transition(State::Active, "(new demand)");
            return true;
        case State::Active: case State::Starting: case State::Stopping:
            return true;
    }
    return true;
}

DemandHandle PipelineManager::acquire_unit(int unit, ConsumerType type, Result* result) {
    std::lock_guard<std::mutex> lk(m_);
    if (shutdown_ || unit < 0 || unit > UNIT_SUB || !units_[unit].configured) {
        if (result) *result = shutdown_ ? Result::busy() : Result::unsupported();
        return DemandHandle();
    }
    if (!ensure_base_locked(type, unit)) {
        if (result) *result = Result::error();
        return DemandHandle();
    }
    Unit& u = units_[unit];
    if (u.grace_armed && u.timer) { u.timer->disarm(); u.grace_armed = false; }
    if (!u.running && !start_unit_locked(unit)) {
        // The base may have just been brought up for this very request; a base
        // with no running unit and no other demand must not stay up.
        if (total_all_locked() == 0 && state_ == State::Active) {
            transition(State::Stopping, "(unit start failed)");
            stop_base_locked();
            transition(State::Failed, last_error_.c_str());
        }
        if (result) *result = Result::error();
        return DemandHandle();
    }
    ++u.demand[(int)type]; ++u.total;
    LOGI(MOD, "demand +1 type=%s unit=%s unit_total=%d all=%d", consumer_name(type), unit_name(unit), u.total, total_all_locked());
    if (result) *result = Result::ok();
    return DemandHandle(this, type, unit);
}

DemandHandle PipelineManager::acquire_base(ConsumerType type, Result* result) {
    std::lock_guard<std::mutex> lk(m_);
    if (shutdown_) { if (result) *result = Result::busy(); return DemandHandle(); }
    if (!ensure_base_locked(type, -1)) {       // -1: bring the base up, start no encoder
        if (result) *result = Result::error();
        return DemandHandle();
    }
    ++ai_demand_;
    LOGI(MOD, "demand +1 type=%s unit=base all=%d", consumer_name(type), total_all_locked());
    if (result) *result = Result::ok();
    return DemandHandle(this, type, UNIT_AI);
}

void PipelineManager::release(ConsumerType type, int unit) {
    std::lock_guard<std::mutex> lk(m_);
    if (unit == UNIT_AI) {
        if (ai_demand_ > 0) --ai_demand_;
        LOGI(MOD, "demand -1 type=%s unit=base all=%d", consumer_name(type), total_all_locked());
        if (!shutdown_ && total_all_locked() == 0 && state_ == State::Active) {
            for (Unit& x : units_) if (x.grace_armed && x.timer) { x.timer->disarm(); x.grace_armed = false; }
            char why[48]; snprintf(why, sizeof why, "timeout=%dms", cfg_.idle_grace_ms);
            transition(State::GraceIdle, why);
            timer_.arm(cfg_.idle_grace_ms);
        }
        return;
    }
    Unit& u = units_[unit];
    if (u.demand[(int)type] > 0) --u.demand[(int)type];
    if (u.total > 0) --u.total;
    LOGI(MOD, "demand -1 type=%s unit=%s unit_total=%d all=%d", consumer_name(type), unit_name(unit), u.total, total_all_locked());
    if (shutdown_) return;
    if (total_all_locked() == 0 && state_ == State::Active) {
        // Base grace, every started unit stays warm - unchanged M4 semantics.
        // A pending per-unit grace makes no sense any more.
        for (Unit& x : units_) if (x.grace_armed && x.timer) { x.timer->disarm(); x.grace_armed = false; }
        char why[48]; snprintf(why, sizeof why, "timeout=%dms", cfg_.idle_grace_ms);
        transition(State::GraceIdle, why);
        timer_.arm(cfg_.idle_grace_ms);
        return;
    }
    // Others still need the base: this unit winds down on its own grace,
    // main included - "shared base, independent units" applies both ways.
    if (u.total == 0 && u.running && state_ == State::Active) {
        if (u.timer) { u.timer->arm(cfg_.idle_grace_ms); u.grace_armed = true; }
        else stop_unit_locked(unit);
    }
}

void PipelineManager::on_grace_timeout() {
    std::lock_guard<std::mutex> lk(m_);
    if (state_ != State::GraceIdle || total_all_locked() > 0) return;
    transition(State::Stopping, nullptr);
    stop_base_locked();
    transition(State::ColdIdle, nullptr);
}

void PipelineManager::on_unit_grace(int unit) {
    std::lock_guard<std::mutex> lk(m_);
    if (unit < 0 || unit > UNIT_SUB) return;
    Unit& u = units_[unit];
    u.grace_armed = false;
    if (u.total == 0 && u.running && state_ == State::Active) stop_unit_locked(unit);
}

void PipelineManager::on_jpeg_grace() {
    std::lock_guard<std::mutex> lk(m_);
    jpeg_.grace_armed = false;
    if (jpeg_.demand == 0 && jpeg_.enc && state_ == State::Active) stop_jpeg_locked();
}

void PipelineManager::shutdown() {
    std::lock_guard<std::mutex> lk(m_);
    shutdown_ = true;
    timer_.disarm();
    for (Unit& u : units_) if (u.grace_armed && u.timer) { u.timer->disarm(); u.grace_armed = false; }
    if (jpeg_.grace_armed && jpeg_.timer) { jpeg_.timer->disarm(); jpeg_.grace_armed = false; }
    if (state_ == State::Active || state_ == State::GraceIdle) {
        transition(State::Stopping, "(shutdown)");
        stop_base_locked();
        transition(State::ColdIdle, nullptr);
    }
}

State PipelineManager::state() const { std::lock_guard<std::mutex> lk(m_); return state_; }

Stats PipelineManager::stats() const {
    std::lock_guard<std::mutex> lk(m_);
    Stats s; s.state = state_; s.total_demand = total_all_locked();
    for (int i = 0; i < (int)ConsumerType::COUNT; ++i) s.demand[i] = units_[UNIT_MAIN].demand[i];
    s.unit_demand[UNIT_MAIN] = units_[UNIT_MAIN].total;
    s.unit_demand[UNIT_SUB]  = units_[UNIT_SUB].total;
    s.unit_demand[UNIT_JPEG] = jpeg_.demand;
    s.unit_demand[UNIT_AI]   = ai_demand_;
    s.unit_active[UNIT_MAIN] = units_[UNIT_MAIN].running;
    s.unit_active[UNIT_SUB]  = units_[UNIT_SUB].running;
    s.unit_active[UNIT_JPEG] = jpeg_.enc != nullptr;
    s.unit_active[UNIT_AI]   = ai_demand_ > 0;
    s.generation = generation_; s.start_count = start_count_; s.stop_count = stop_count_;
    s.failed_count = failed_count_; s.restart_count = restart_count_; s.sub_restart_count = sub_restart_count_;
    s.last_error = last_error_;
    s.frames_this_run = units_[UNIT_MAIN].frames.load();
    // total_bytes is a plain u64 guarded by each unit's win_m (avoids a 64-bit
    // atomic that would need libatomic on MIPS); take a brief per-unit lock.
    for (int u = UNIT_MAIN; u <= UNIT_SUB; ++u) {
        std::lock_guard<std::mutex> wl(units_[u].win_m);
        s.total_bytes[u] = units_[u].total_bytes;
    }
    s.jpeg_captures = jpeg_.captures; s.jpeg_failures = jpeg_.failures; s.jpeg_last_capture_ms = jpeg_.last_capture_ms;
    return s;
}

Measurement PipelineManager::measurement() const { return measurement_unit(UNIT_MAIN); }

Measurement PipelineManager::measurement_unit(int unit) const {
    if (unit < 0 || unit > UNIT_SUB) return Measurement{};
    const Unit& u = units_[unit];
    std::lock_guard<std::mutex> lk(u.win_m);
    Measurement m = u.last_win; m.dropped_frames = u.dropped.load();
    return m;
}

EffectiveStream PipelineManager::stream() const { return stream_unit(UNIT_MAIN); }

EffectiveStream PipelineManager::stream_unit(int unit) const {
    std::lock_guard<std::mutex> lk(m_);
    if (unit < 0 || unit > UNIT_SUB) return EffectiveStream{};
    return units_[unit].stream;
}

bool PipelineManager::unit_configured(int unit) const {
    std::lock_guard<std::mutex> lk(m_);
    if (unit == UNIT_JPEG) return jpeg_.configured;
    return unit >= 0 && unit <= UNIT_SUB && units_[unit].configured;
}

bool PipelineManager::unit_active(int unit) const {
    std::lock_guard<std::mutex> lk(m_);
    if (unit == UNIT_JPEG) return jpeg_.enc != nullptr;
    if (unit == UNIT_AI)   return ai_demand_ > 0;   // base-only holder, never its own encoder
    return unit >= 0 && unit <= UNIT_SUB && units_[unit].running;
}

// ---- M5 controls -------------------------------------------------------------
Result PipelineManager::update_stream(const EffectiveStream& s, bool restart_if_running, std::string& err) {
    std::lock_guard<std::mutex> lk(m_);
    units_[UNIT_MAIN].stream = s;
    bool running = (state_ == State::Active || state_ == State::GraceIdle);
    if (!running || !restart_if_running) return Result::ok();     // cold: takes effect at next start
    // controlled restart: same demand, same generation counter semantics
    State back = state_;
    transition(State::Stopping, "(restart: pipeline-restart setting)");
    stop_base_locked();
    ++restart_count_;
    transition(State::Starting, "(restart)");
    if (!start_base_locked(UNIT_MAIN)) {
        err = last_error_;
        transition(State::Failed, last_error_.c_str());
        return Result::error();
    }
    transition(back, "(restart complete)");
    if (back == State::GraceIdle) timer_.arm(cfg_.idle_grace_ms);   // keep the pending grace
    return Result::ok();
}

Result PipelineManager::update_sub_stream(const EffectiveStream& s, bool enabled, std::string& err) {
    std::lock_guard<std::mutex> lk(m_);
    Unit& u = units_[UNIT_SUB];
    if (!enabled) {
        if (u.running) stop_unit_locked(UNIT_SUB);
        u.configured = false;
        return Result::ok();
    }
    if (!u.configured) { err = "substream not configured (no hub/timer wired)"; return Result::unsupported(); }
    u.stream = s;
    if (!u.running) return Result::ok();          // cold unit: applies at next unit start
    stop_unit_locked(UNIT_SUB);
    ++sub_restart_count_;
    if (!start_unit_locked(UNIT_SUB)) {
        err = last_error_;
        return Result::error();
    }
    return Result::ok();
}

Result PipelineManager::live_bitrate(int kbps, int& effective) {
    std::lock_guard<std::mutex> lk(m_);
    effective = -1;
    Unit& u = units_[UNIT_MAIN];
    if (!(state_ == State::Active || state_ == State::GraceIdle) || !u.enc) return Result::busy();
    Result r = u.enc->set_bitrate(kbps, effective);
    if (r) u.stream.bitrate_kbps = effective > 0 ? effective : kbps;
    return r;
}

Result PipelineManager::live_encoder_fps(int fps, int& effective) {
    std::lock_guard<std::mutex> lk(m_);
    effective = -1;
    Unit& u = units_[UNIT_MAIN];
    if (!(state_ == State::Active || state_ == State::GraceIdle) || !u.enc) return Result::busy();
    return u.enc->set_fps(fps, effective);
}

Result PipelineManager::live_sensor_fps(int fps, int& effective) {
    std::lock_guard<std::mutex> lk(m_);
    effective = -1;
    if (!(state_ == State::Active || state_ == State::GraceIdle)) return Result::busy();
    return platform_.set_sensor_fps(fps, effective);
}

Result PipelineManager::read_sensor_fps(int& fps) {
    std::lock_guard<std::mutex> lk(m_);
    fps = -1;
    if (!(state_ == State::Active || state_ == State::GraceIdle)) return Result::busy();
    return platform_.get_sensor_fps(fps);
}

void PipelineManager::set_sensor_fps_target(int fps) { std::lock_guard<std::mutex> lk(m_); sensor_fps_target_ = fps; }

Result PipelineManager::live_gop(int frames, int& effective) {
    std::lock_guard<std::mutex> lk(m_);
    effective = -1;
    if (frames < 1 || frames > 1000) return Result::error();
    Unit& u = units_[UNIT_MAIN];
    if (!(state_ == State::Active || state_ == State::GraceIdle) || !u.enc) return Result::busy();
    Result r = u.enc->set_gop(frames, effective);
    // A successful set means the encoder accepted this GOP and will use it.
    // Encoders may still report the previous length for up to one GOP (the
    // Ingenic SDK does), so that transient must not become the effective
    // value: it would make /state report the old GOP forever and let a later
    // restart recreate the channel with it, silently undoing the change.
    if (r) { u.stream.gop = frames; effective = frames; }
    return r;
}

Result PipelineManager::live_image(ImageControl c, int value, int& effective) {
    std::lock_guard<std::mutex> lk(m_);
    effective = -1;
    if (!(state_ == State::Active || state_ == State::GraceIdle)) return Result::busy();
    IImageControl* image = platform_.image();
    return image ? image->set(c, value, effective) : Result::unsupported();
}

Result PipelineManager::read_exposure(ExposureReadback& out) {
    std::lock_guard<std::mutex> lk(m_);
    out = ExposureReadback{};
    if (!(state_ == State::Active || state_ == State::GraceIdle)) return Result::busy();
    IImageControl* image = platform_.image();
    return image ? image->exposure(out) : Result::unsupported();
}

void PipelineManager::request_idr(int unit) {
    std::lock_guard<std::mutex> lk(m_);
    if (unit < 0 || unit > UNIT_SUB) return;
    Unit& u = units_[unit];
    if ((state_ == State::Active || state_ == State::GraceIdle) && u.enc) u.enc->request_idr();
}

// ---- snapshot ----------------------------------------------------------------
Result PipelineManager::snapshot(std::vector<uint8_t>& out, std::string& err, int timeout_ms) {
    {
        std::lock_guard<std::mutex> lk(m_);
        if (shutdown_) { err = "shutting down"; return Result::busy(); }
        if (!jpeg_.configured) { err = "jpeg not configured on this platform"; return Result::unsupported(); }
        if (!ensure_base_locked(ConsumerType::Snapshot, UNIT_JPEG)) { err = last_error_; return Result::error(); }
        if (jpeg_.grace_armed && jpeg_.timer) { jpeg_.timer->disarm(); jpeg_.grace_armed = false; }
        if (!jpeg_.enc && !start_jpeg_locked()) {
            err = last_error_;
            if (total_all_locked() == 0 && state_ == State::Active) {
                transition(State::Stopping, "(jpeg start failed)");
                stop_base_locked();
                transition(State::Failed, last_error_.c_str());
            }
            return Result::error();
        }
        ++jpeg_.demand;
        LOGI(MOD, "demand +1 type=snapshot unit=jpeg all=%d", total_all_locked());
    }

    // The capture itself runs without the manager lock: it can block for a
    // frame interval, and the H.264 paths must not stall behind it. snap_m_
    // serialises concurrent snapshot requests; within cache_ms they all get
    // the same image instead of hammering the hardware.
    Result r = Result::ok();
    {
        std::lock_guard<std::mutex> cap(snap_m_);
        int64_t now = mono_us();
        bool fresh = jpeg_.cache_at_us > 0 && (now - jpeg_.cache_at_us) < (int64_t)jpeg_.cache_ms * 1000 && !jpeg_.cache.empty();
        if (fresh) {
            out = jpeg_.cache;
        } else {
            IJpegEncoder* enc = jpeg_.enc.get();     // stays alive: demand > 0
            int64_t t0 = mono_us();
            r = enc ? enc->capture(out, timeout_ms) : Result::busy();
            std::lock_guard<std::mutex> lk(m_);
            jpeg_.last_capture_ms = (mono_us() - t0) / 1000;
            if (r) { ++jpeg_.captures; jpeg_.cache = out; jpeg_.cache_at_us = mono_us(); }
            else   { ++jpeg_.failures; err = "jpeg capture failed"; }
        }
    }

    {
        std::lock_guard<std::mutex> lk(m_);
        if (jpeg_.demand > 0) --jpeg_.demand;
        LOGI(MOD, "demand -1 type=snapshot unit=jpeg all=%d", total_all_locked());
        if (!shutdown_) {
            if (total_all_locked() == 0 && state_ == State::Active) {
                for (Unit& x : units_) if (x.grace_armed && x.timer) { x.timer->disarm(); x.grace_armed = false; }
                char why[48]; snprintf(why, sizeof why, "timeout=%dms", cfg_.idle_grace_ms);
                transition(State::GraceIdle, why);
                timer_.arm(cfg_.idle_grace_ms);
            } else if (jpeg_.demand == 0 && jpeg_.enc && state_ == State::Active) {
                if (jpeg_.timer) { jpeg_.timer->arm(jpeg_.grace_ms); jpeg_.grace_armed = true; }
                else stop_jpeg_locked();
            }
        }
    }
    return r;
}

// ---- media chain (m_ held) --------------------------------------------------
bool PipelineManager::start_base_locked(int first_unit) {
    ++start_count_;
    last_error_.clear();
    Result r = platform_.bring_up();
    if (!r) {
        last_error_ = "platform bring-up failed (" + std::to_string(r.code) + ")";
        LOGE(MOD, "start: %s - rolling back", last_error_.c_str());
        stop_base_locked();
        ++failed_count_;
        return false;
    }

    // Start the requesting unit plus every unit that still holds demand - a
    // controlled base restart must bring the substream back too, not just the
    // main stream. Order main before sub, so the pre-M8 call sequence (and the
    // tests that pin it) stays identical when only the main stream exists.
    for (int unit = UNIT_MAIN; unit <= UNIT_SUB; ++unit) {
        Unit& u = units_[unit];
        bool wanted = (unit == first_unit) || (u.configured && u.total > 0);
        if (!wanted || u.running) continue;
        if (!start_unit_locked(unit)) {
            stop_base_locked();
            ++failed_count_;
            return false;
        }
    }

    if (sensor_fps_target_ > 0) {                     // sensor rate is a separate knob from the stream rate
        int eff = -1; Result sr = platform_.set_sensor_fps(sensor_fps_target_, eff);
        if (sr) LOGI(MOD, "sensor fps %d applied (effective %d)", sensor_fps_target_, eff);
        else LOGW(MOD, "sensor fps %d not applied (%s)", sensor_fps_target_, status_name(sr.status));
    }

    if (post_start_) post_start_();

    ++generation_;
    const EffectiveStream& s = units_[UNIT_MAIN].stream;
    LOGI(MOD, "pipeline generation %u running (%dx%d@%d, %d kbps)", generation_, s.width, s.height, s.fps, s.bitrate_kbps);
    return true;
}

void PipelineManager::stop_base_locked() {
    stop_unit_locked(UNIT_SUB);
    stop_unit_locked(UNIT_MAIN);
    stop_jpeg_locked();
    platform_.tear_down();
    ++stop_count_;
    LOGI(MOD, "pipeline stopped (%u frames this run, pool exhausted %u)",
         units_[UNIT_MAIN].frames.load(), units_[UNIT_MAIN].pool ? units_[UNIT_MAIN].pool->exhausted() : 0);
}

bool PipelineManager::start_unit_locked(int unit) {
    Unit& u = units_[unit];
    if (u.running) return true;
    auto fail = [&](const char* stage, int code) {
        last_error_ = std::string(unit_name(unit)) + " " + stage + " failed (" + std::to_string(code) + ")";
        LOGE(MOD, "start: %s - rolling back this unit", last_error_.c_str());
        stop_unit_locked(unit);
        return false;
    };
    u.fs = platform_.create_framesource(unit, u.stream);
    if (!u.fs) return fail("framesource create", -1);
    u.enc = platform_.create_encoder(unit, u.stream);
    if (!u.enc) return fail("encoder create", -1);
    Result r = platform_.bind(*u.fs, *u.enc);
    if (!r) return fail("bind", r.code);
    u.bound = true;
    r = u.fs->enable();
    if (!r) return fail("framesource enable", r.code);
    u.fs_enabled = true;
    r = u.enc->start();
    if (!r) return fail("encoder start", r.code);
    u.enc_started = true;

    u.quit = false; u.frames = 0; u.dropped = 0;
    {
        std::lock_guard<std::mutex> wl(u.win_m);
        u.last_win = Measurement{}; u.win_start_us = 0; u.win_frames = 0; u.win_bytes = 0;
    }
    u.thread = std::thread([this, &u] { capture_loop(u); });
    u.running = true;
    LOGI(MOD, "unit %s running (%dx%d@%d, %d kbps)", unit_name(unit), u.stream.width, u.stream.height, u.stream.fps, u.stream.bitrate_kbps);
    return true;
}

void PipelineManager::stop_unit_locked(int unit) {
    Unit& u = units_[unit];
    u.quit = true;
    if (u.thread.joinable()) u.thread.join();
    // Release only what was actually acquired. `bound` was already tracked;
    // `enable` and `start` were not, so a unit that failed at encoder-create
    // used to call disable() on a framesource that had never been enabled.
    // The Ingenic adapter happens to guard that internally, but the lifecycle
    // must not depend on an adapter being forgiving - and an unwind that logs
    // steps it never performed is unreadable exactly when it matters.
    if (u.enc && u.enc_started) u.enc->stop();
    if (u.fs  && u.fs_enabled)  u.fs->disable();
    if (u.bound && u.fs && u.enc) platform_.unbind(*u.fs, *u.enc);
    u.enc_started = false;
    u.fs_enabled  = false;
    u.bound = false;
    u.enc.reset();
    u.fs.reset();
    if (u.running) LOGI(MOD, "unit %s stopped (%u frames)", unit_name(unit), u.frames.load());
    u.running = false;
}

bool PipelineManager::start_jpeg_locked() {
    if (jpeg_.enc) return true;
    JpegParams p = jpeg_.params;
    if (p.width <= 0 || p.height <= 0) { p.width = units_[UNIT_MAIN].stream.width; p.height = units_[UNIT_MAIN].stream.height; }
    jpeg_.enc = platform_.create_jpeg(UNIT_JPEG, p);
    if (!jpeg_.enc) {
        last_error_ = "jpeg encoder create failed";
        LOGE(MOD, "start: %s", last_error_.c_str());
        return false;
    }
    LOGI(MOD, "unit jpeg running (%dx%d q=%d)", p.width, p.height, p.quality);
    return true;
}

void PipelineManager::stop_jpeg_locked() {
    if (!jpeg_.enc) return;
    jpeg_.enc.reset();
    jpeg_.cache.clear();
    jpeg_.cache_at_us = -1;
    LOGI(MOD, "unit jpeg stopped (%u captures, %u failures)", jpeg_.captures, jpeg_.failures);
}

void PipelineManager::capture_loop(Unit& u) {
    unsigned timeouts = 0;
    while (!u.quit) {
        auto au = u.pool->acquire();
        if (!au) {
            AccessUnit scratch; Result r = u.enc->fetch(scratch, cfg_.poll_timeout_ms);
            if (r) { unsigned d = ++u.dropped; if ((d % 50) == 1) LOGW(MOD, "pool exhausted - dropped %u frames", d); }
            continue;
        }
        Result r = u.enc->fetch(*au, cfg_.poll_timeout_ms);
        if (r.status == Status::Timeout) {
            if ((++timeouts % 20) == 1) LOGW(MOD, "encoder idle (no frame for %u polls)", timeouts);
            continue;
        }
        if (!r) { LOGW(MOD, "fetch failed (%d)", r.code); continue; }
        timeouts = 0;
        struct timespec mt{}; clock_gettime(CLOCK_MONOTONIC, &mt);
        au->fetched_us = (int64_t)mt.tv_sec * 1000000 + mt.tv_nsec / 1000;
        // IMP timestamps and IMP_System_GetTimeStamp are the same platform
        // time domain. Reject nonsensical deltas rather than publishing a
        // made-up latency number on adapters that cannot guarantee that.
        int64_t platform_now = platform_.timestamp_us();
        if (au->pts_us > 0 && platform_now >= au->pts_us)
            u.hub->record_capture_to_out(platform_now - au->pts_us);
        au->seq = u.seq++;
        unsigned n = ++u.frames;
        if (n == 1) LOGI(MOD, "first frame: %lu bytes key=%d pts=%lld", (unsigned long)au->data.size(), (int)au->key, (long long)au->pts_us);
        // 1 s measurement window from frame events (no polling thread)
        {
            std::lock_guard<std::mutex> wl(u.win_m);
            if (u.win_start_us == 0) u.win_start_us = au->pts_us;
            ++u.win_frames; u.win_bytes += au->data.size();
            u.total_bytes += au->data.size();      // monotonic, for /metrics venc*_rcvd_bytes

            int64_t span = au->pts_us - u.win_start_us;
            if (span >= 1000000) {
                u.last_win.valid = true;
                u.last_win.encoded_fps  = (double)u.win_frames * 1e6 / (double)span;
                u.last_win.bitrate_kbps = (double)u.win_bytes * 8.0 / 1000.0 * 1e6 / (double)span;
                u.win_start_us = au->pts_us; u.win_frames = 0; u.win_bytes = 0;
            }
        }
        u.hub->publish(au);
    }
}

}} // namespace machino::lifecycle
