#include "core/lifecycle/pipeline_manager.hpp"
#include "core/log.hpp"
#include <ctime>

namespace machino { namespace lifecycle {

static const char* MOD = "lifecycle";

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
    m->release(type_);
}

PipelineManager::PipelineManager(IPlatform& platform, const EffectiveStream& stream, const LifecycleConfig& cfg,
                                 IGraceTimer& timer, StreamHub& hub)
    // 16 slots > the sum of all consumer queue depths (StreamHub sinks hold at most 4 each):
    // a stalled consumer drops its own oldest frames, it can never starve the capture path.
    : platform_(platform), stream_(stream), cfg_(cfg), timer_(timer), hub_(hub), pool_(16, 128 * 1024) {}

PipelineManager::~PipelineManager() { shutdown(); }

void PipelineManager::transition(State to, const char* why) {
    LOGI(MOD, "%s -> %s%s%s", state_name(state_), state_name(to), why ? " " : "", why ? why : "");
    State from = state_;
    state_ = to;
    if (listener_) listener_(from, to);
}

// ---- demand ---------------------------------------------------------------
DemandHandle PipelineManager::acquire(ConsumerType type, Result* result) {
    std::lock_guard<std::mutex> lk(m_);
    if (shutdown_) { if (result) *result = Result::busy(); return DemandHandle(); }
    switch (state_) {
        case State::ColdIdle:
        case State::Failed: {
            char why[48]; snprintf(why, sizeof why, "(consumer=%s)", consumer_name(type));
            transition(State::Starting, why);
            if (!start_locked()) {
                transition(State::Failed, last_error_.c_str());
                if (result) *result = Result::error();
                return DemandHandle();
            }
            transition(State::Active, nullptr);
            break;
        }
        case State::GraceIdle:
            timer_.disarm();
            transition(State::Active, "(new demand)");
            break;
        case State::Active: case State::Starting: case State::Stopping:
            break;
    }
    ++demand_[(int)type]; ++total_;
    LOGI(MOD, "demand +1 type=%s total=%d", consumer_name(type), total_);
    if (result) *result = Result::ok();
    return DemandHandle(this, type);
}

void PipelineManager::release(ConsumerType type) {
    std::lock_guard<std::mutex> lk(m_);
    if (demand_[(int)type] > 0) --demand_[(int)type];
    if (total_ > 0) --total_;
    LOGI(MOD, "demand -1 type=%s total=%d", consumer_name(type), total_);
    if (total_ == 0 && state_ == State::Active && !shutdown_) {
        char why[48]; snprintf(why, sizeof why, "timeout=%dms", cfg_.idle_grace_ms);
        transition(State::GraceIdle, why);
        timer_.arm(cfg_.idle_grace_ms);
    }
}

void PipelineManager::on_grace_timeout() {
    std::lock_guard<std::mutex> lk(m_);
    if (state_ != State::GraceIdle || total_ > 0) return;
    transition(State::Stopping, nullptr);
    stop_locked();
    transition(State::ColdIdle, nullptr);
}

void PipelineManager::shutdown() {
    std::lock_guard<std::mutex> lk(m_);
    shutdown_ = true;
    timer_.disarm();
    if (state_ == State::Active || state_ == State::GraceIdle) {
        transition(State::Stopping, "(shutdown)");
        stop_locked();
        transition(State::ColdIdle, nullptr);
    }
}

State PipelineManager::state() const { std::lock_guard<std::mutex> lk(m_); return state_; }

Stats PipelineManager::stats() const {
    std::lock_guard<std::mutex> lk(m_);
    Stats s; s.state = state_; s.total_demand = total_;
    for (int i = 0; i < (int)ConsumerType::COUNT; ++i) s.demand[i] = demand_[i];
    s.generation = generation_; s.start_count = start_count_; s.stop_count = stop_count_;
    s.failed_count = failed_count_; s.restart_count = restart_count_; s.last_error = last_error_;
    s.frames_this_run = frames_.load();
    return s;
}

Measurement PipelineManager::measurement() const {
    std::lock_guard<std::mutex> lk(win_m_);
    Measurement m = last_win_; m.dropped_frames = dropped_.load();
    return m;
}

EffectiveStream PipelineManager::stream() const { std::lock_guard<std::mutex> lk(m_); return stream_; }

// ---- M5 controls -------------------------------------------------------------
Result PipelineManager::update_stream(const EffectiveStream& s, bool restart_if_running, std::string& err) {
    std::lock_guard<std::mutex> lk(m_);
    stream_ = s;
    bool running = (state_ == State::Active || state_ == State::GraceIdle);
    if (!running || !restart_if_running) return Result::ok();     // cold: takes effect at next start
    // controlled restart: same demand, same generation counter semantics
    State back = state_;
    transition(State::Stopping, "(restart: pipeline-restart setting)");
    stop_locked();
    ++restart_count_;
    transition(State::Starting, "(restart)");
    if (!start_locked()) {
        err = last_error_;
        transition(State::Failed, last_error_.c_str());
        return Result::error();
    }
    transition(back, "(restart complete)");
    if (back == State::GraceIdle) timer_.arm(cfg_.idle_grace_ms);   // keep the pending grace
    return Result::ok();
}

Result PipelineManager::live_bitrate(int kbps, int& effective) {
    std::lock_guard<std::mutex> lk(m_);
    effective = -1;
    if (!(state_ == State::Active || state_ == State::GraceIdle) || !enc_) return Result::busy();
    Result r = enc_->set_bitrate(kbps, effective);
    if (r) stream_.bitrate_kbps = effective > 0 ? effective : kbps;
    return r;
}

Result PipelineManager::live_encoder_fps(int fps, int& effective) {
    std::lock_guard<std::mutex> lk(m_);
    effective = -1;
    if (!(state_ == State::Active || state_ == State::GraceIdle) || !enc_) return Result::busy();
    return enc_->set_fps(fps, effective);
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
    if (!(state_ == State::Active || state_ == State::GraceIdle) || !enc_) return Result::busy();
    Result r = enc_->set_gop(frames, effective);
    // A successful set means the encoder accepted this GOP and will use it.
    // Encoders may still report the previous length for up to one GOP (the
    // Ingenic SDK does), so that transient must not become the effective
    // value: it would make /state report the old GOP forever and let a later
    // restart recreate the channel with it, silently undoing the change.
    if (r) { stream_.gop = frames; effective = frames; }
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

void PipelineManager::request_idr() {
    std::lock_guard<std::mutex> lk(m_);
    if ((state_ == State::Active || state_ == State::GraceIdle) && enc_) enc_->request_idr();
}

// ---- media chain (m_ held) --------------------------------------------------
bool PipelineManager::start_locked() {
    ++start_count_;
    last_error_.clear();
    auto fail = [&](const char* stage, int code) {
        last_error_ = std::string(stage) + " failed (" + std::to_string(code) + ")";
        LOGE(MOD, "start: %s - rolling back", last_error_.c_str());
        stop_locked();
        ++failed_count_;
        return false;
    };
    Result r = platform_.bring_up();
    if (!r) return fail("platform bring-up", r.code);
    fs_ = platform_.create_framesource(0, stream_);
    if (!fs_) return fail("framesource create", -1);
    enc_ = platform_.create_encoder(0, stream_);
    if (!enc_) return fail("encoder create", -1);
    r = platform_.bind(*fs_, *enc_);
    if (!r) return fail("bind", r.code);
    bound_ = true;
    r = fs_->enable();
    if (!r) return fail("framesource enable", r.code);
    r = enc_->start();
    if (!r) return fail("encoder start", r.code);

    if (sensor_fps_target_ > 0) {                     // sensor rate is a separate knob from the stream rate
        int eff = -1; Result sr = platform_.set_sensor_fps(sensor_fps_target_, eff);
        if (sr) LOGI(MOD, "sensor fps %d applied (effective %d)", sensor_fps_target_, eff);
        else LOGW(MOD, "sensor fps %d not applied (%s)", sensor_fps_target_, status_name(sr.status));
    }

    if (post_start_) post_start_();

    quit_ = false; frames_ = 0; dropped_ = 0;
    { std::lock_guard<std::mutex> wl(win_m_); last_win_ = Measurement{}; win_start_us_ = 0; win_frames_ = 0; win_bytes_ = 0; }
    thread_ = std::thread([this] { capture_loop(); });
    ++generation_;
    LOGI(MOD, "pipeline generation %u running (%dx%d@%d, %d kbps)", generation_, stream_.width, stream_.height,
         stream_.fps, stream_.bitrate_kbps);
    return true;
}

void PipelineManager::stop_locked() {
    quit_ = true;
    if (thread_.joinable()) thread_.join();
    if (enc_) enc_->stop();
    if (fs_)  fs_->disable();
    if (bound_ && fs_ && enc_) platform_.unbind(*fs_, *enc_);
    bound_ = false;
    enc_.reset();
    fs_.reset();
    platform_.tear_down();
    ++stop_count_;
    LOGI(MOD, "pipeline stopped (%u frames this run, pool exhausted %u)", frames_.load(), pool_.exhausted());
}

void PipelineManager::capture_loop() {
    unsigned timeouts = 0;
    while (!quit_) {
        auto au = pool_.acquire();
        if (!au) {
            AccessUnit scratch; Result r = enc_->fetch(scratch, cfg_.poll_timeout_ms);
            if (r) { unsigned d = ++dropped_; if ((d % 50) == 1) LOGW(MOD, "pool exhausted - dropped %u frames", d); }
            continue;
        }
        Result r = enc_->fetch(*au, cfg_.poll_timeout_ms);
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
            hub_.record_capture_to_out(platform_now - au->pts_us);
        au->seq = seq_++;
        unsigned n = ++frames_;
        if (n == 1) LOGI(MOD, "first frame: %lu bytes key=%d pts=%lld", (unsigned long)au->data.size(), (int)au->key, (long long)au->pts_us);
        // 1 s measurement window from frame events (no polling thread)
        {
            std::lock_guard<std::mutex> wl(win_m_);
            if (win_start_us_ == 0) win_start_us_ = au->pts_us;
            ++win_frames_; win_bytes_ += au->data.size();
            int64_t span = au->pts_us - win_start_us_;
            if (span >= 1000000) {
                last_win_.valid = true;
                last_win_.encoded_fps  = (double)win_frames_ * 1e6 / (double)span;
                last_win_.bitrate_kbps = (double)win_bytes_ * 8.0 / 1000.0 * 1e6 / (double)span;
                win_start_us_ = au->pts_us; win_frames_ = 0; win_bytes_ = 0;
            }
        }
        hub_.publish(au);
    }
}

}} // namespace machino::lifecycle
