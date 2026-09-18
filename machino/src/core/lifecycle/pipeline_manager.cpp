#include "core/lifecycle/pipeline_manager.hpp"
#include "core/log.hpp"

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
    : platform_(platform), stream_(stream), cfg_(cfg), timer_(timer), hub_(hub), pool_(8, 256 * 1024) {}

PipelineManager::~PipelineManager() { shutdown(); }

void PipelineManager::transition(State to, const char* why) {
    LOGI(MOD, "%s -> %s%s%s", state_name(state_), state_name(to), why ? " " : "", why ? why : "");
    state_ = to;
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
                return DemandHandle();                   // no demand registered
            }
            transition(State::Active, nullptr);
            break;
        }
        case State::GraceIdle:
            timer_.disarm();
            transition(State::Active, "(new demand)");
            break;
        case State::Active:
            break;
        case State::Starting:
        case State::Stopping:
            // unreachable: transitions complete while m_ is held
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
    if (state_ != State::GraceIdle || total_ > 0) return;   // stale expiry: demand returned meanwhile
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
    s.failed_count = failed_count_; s.last_error = last_error_; s.frames_this_run = frames_.load();
    return s;
}

// ---- media chain (m_ held) --------------------------------------------------
// Proven order: platform bring-up -> FrameSource -> Encoder -> Bind -> FS enable
// -> StartRecvPic -> capture thread. Any failure rolls back what exists.
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

    quit_ = false; frames_ = 0;
    thread_ = std::thread([this] { capture_loop(); });
    ++generation_;
    LOGI(MOD, "pipeline generation %u running (%dx%d@%d, %d kbps)", generation_, stream_.width, stream_.height,
         stream_.fps, stream_.bitrate_kbps);
    return true;
}

// Reverse order. Safe from any partially started state.
void PipelineManager::stop_locked() {
    quit_ = true;
    if (thread_.joinable()) thread_.join();
    if (enc_) enc_->stop();                                  // StopRecvPic
    if (fs_)  fs_->disable();                                // FrameSource_DisableChn
    if (bound_ && fs_ && enc_) platform_.unbind(*fs_, *enc_);
    bound_ = false;
    enc_.reset();                                            // UnRegister/DestroyChn, DestroyGroup
    fs_.reset();                                             // FrameSource_DestroyChn
    platform_.tear_down();                                   // tuning, System_Exit, sensor, ISP_Close
    ++stop_count_;
    LOGI(MOD, "pipeline stopped (%u frames this run, pool exhausted %u)", frames_.load(), pool_.exhausted());
}

void PipelineManager::capture_loop() {
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
}

}} // namespace machino::lifecycle
