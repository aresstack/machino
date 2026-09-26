#include "core/detection/detection_service.hpp"
#include "core/json.hpp"
#include "core/log.hpp"
#include <ctime>

namespace machino { namespace detection {

static const char* MOD = "AI";

const char* ai_state_name(AiState s) {
    switch (s) {
        case AiState::Disabled: return "disabled";
        case AiState::Starting: return "starting";
        case AiState::Active:   return "active";
        case AiState::Error:    return "error";
    }
    return "?";
}

static int64_t now_ms() { struct timespec ts{}; clock_gettime(CLOCK_REALTIME, &ts); return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000; }

DetectionService::DetectionService(lifecycle::PipelineManager& pipeline, IPlatform& platform, EventBus& bus, const AiConfig& cfg)
    : pipeline_(pipeline), platform_(platform), bus_(bus), cfg_(cfg) {
    if (cfg_.enabled) { std::lock_guard<std::mutex> lk(m_); start_locked(); }
}

DetectionService::~DetectionService() { shutdown(); }

void DetectionService::shutdown() {
    std::lock_guard<std::mutex> lk(m_);
    stop_locked();
}

AiState DetectionService::state() const { std::lock_guard<std::mutex> lk(m_); return state_; }

// Start the detector: base-only demand (sensor/ISP, no encoder), then the
// backend. A missing/unavailable backend is an AI error, never fatal.
Result DetectionService::start_locked() {
    if (state_ == AiState::Active || state_ == AiState::Starting) return Result::ok();
    state_ = AiState::Starting; last_error_.clear();

    Result dr; demand_ = pipeline_.acquire_base(lifecycle::ConsumerType::Ai, &dr);
    if (!demand_.active()) {
        state_ = AiState::Error; last_error_ = "base did not come up";
        LOGW(MOD, "detector '%s': %s", cfg_.detector.c_str(), last_error_.c_str());
        return Result::error();
    }
    DetectorParams p; p.detector = cfg_.detector; p.inference_fps = cfg_.inference_fps; p.model_path = cfg_.model_path;
    // UNIT_AI is the detector's own channel index: its analysis FrameSource must
    // not collide with main(0)/sub(1)/jpeg(2). The adapter maps it to real IMP
    // group/channel numbers.
    det_ = platform_.create_detector(lifecycle::UNIT_AI, p);
    if (!det_) {
        demand_.release();
        state_ = AiState::Error; last_error_ = "detector backend '" + cfg_.detector + "' unavailable on this platform";
        LOGW(MOD, "%s", last_error_.c_str());
        return Result::unsupported();
    }
    backend_ = det_->backend();
    Result sr = det_->start();
    if (!sr) {
        det_.reset(); demand_.release();
        state_ = AiState::Error; last_error_ = "detector start failed";
        LOGW(MOD, "detector '%s' start failed", cfg_.detector.c_str());
        return sr;
    }
    { std::lock_guard<std::mutex> lk(tel_m_); completed_ = failed_ = detections_ = 0; win_start_ms_ = now_ms(); win_completed_ = 0; eff_fps_ = 0; motion_now_ = false; last_inference_ms_ = last_detection_ms_ = -1; last_dur_ms_ = -1; dur_sum_ms_ = 0; dur_n_ = 0; }
    quit_ = false;
    thread_ = std::thread([this] { run(); });
    state_ = AiState::Active;
    LOGI(MOD, "detector active: %s (backend %s, %d fps)", cfg_.detector.c_str(), backend_.c_str(), cfg_.inference_fps);
    Json j = Json::object(); j.set("state", Json::string("active")); j.set("detector", Json::string(cfg_.detector)); j.set("backend", Json::string(backend_));
    bus_.publish("ai", j.dump());
    return Result::ok();
}

void DetectionService::stop_locked() {
    quit_.store(true, std::memory_order_release);
    // run() only touches tel_m_ and atomics, never m_, so releasing m_ across
    // the join cannot deadlock; all control callers hold m_ so stops serialise.
    if (thread_.joinable()) { m_.unlock(); thread_.join(); m_.lock(); }
    if (det_) { det_->stop(); det_.reset(); }
    demand_.release();
    backend_.clear();
    if (state_ != AiState::Error) state_ = AiState::Disabled;
}

void DetectionService::run() {
    bool prev_motion = false;
    while (!quit_.load(std::memory_order_acquire)) {
        DetectionResult r;
        Result pr = det_->poll(r, 500);
        if (quit_.load(std::memory_order_acquire)) break;
        if (pr.status == Status::Timeout) continue;          // no activity is normal, not a failure
        int64_t t = now_ms();
        if (!pr) {
            std::lock_guard<std::mutex> lk(tel_m_); ++failed_;
            continue;
        }
        {
            std::lock_guard<std::mutex> lk(tel_m_);
            ++completed_; ++win_completed_; last_inference_ms_ = t;
            if (win_start_ms_ == 0) win_start_ms_ = t;
            int64_t span = t - win_start_ms_;
            if (span >= 1000) { eff_fps_ = (double)win_completed_ * 1000.0 / (double)span; win_start_ms_ = t; win_completed_ = 0; }
            if (r.any()) { ++detections_; last_detection_ms_ = t; }
            if (r.infer_duration_ms >= 0) {
                last_dur_ms_ = r.infer_duration_ms;
                dur_sum_ms_ += (double)r.infer_duration_ms;
                ++dur_n_;
            }
            motion_now_ = r.motion;
        }
        // Emit on a detection or on a motion on/off transition, never per empty frame.
        if (r.any() || r.motion != prev_motion) {
            Json j = Json::object();
            j.set("motion", Json::boolean(r.motion));
            j.set("motion_level", Json::integer(r.motion_level));
            Json arr = Json::array();
            for (const auto& d : r.detections) {
                Json o = Json::object();
                o.set("label", Json::string(d.label.empty() ? "object" : d.label));
                o.set("confidence", Json::integer(d.confidence));
                if (d.class_id >= 0) o.set("class_id", Json::integer(d.class_id));
                // Die Box gehoert INS Ereignis: ein Abonnent, der auf eine
                // Person reagieren will, braucht WO, nicht nur DASS. Nur wenn
                // sie echt ist -- Motion liefert Ganzbild ({}), und eine
                // erfundene 0/0/0/0-Box waere eine Aussage.
                if (d.box.w > 0.f && d.box.h > 0.f) {
                    Json b = Json::object();
                    b.set("x", Json::number(d.box.x));
                    b.set("y", Json::number(d.box.y));
                    b.set("w", Json::number(d.box.w));
                    b.set("h", Json::number(d.box.h));
                    o.set("box", b);
                }
                arr.push(o);
            }
            j.set("detections", arr);
            bus_.publish("detection", j.dump());
        }
        prev_motion = r.motion;
    }
}

Result DetectionService::set_enabled(bool on) {
    std::lock_guard<std::mutex> lk(m_);
    cfg_.enabled = on;
    if (on) return start_locked();
    stop_locked();
    return Result::ok();
}

Result DetectionService::set_detector(const std::string& name) {
    std::lock_guard<std::mutex> lk(m_);
    cfg_.detector = name;
    if (state_ == AiState::Active || state_ == AiState::Starting) { stop_locked(); return start_locked(); }
    return Result::ok();
}

Result DetectionService::set_inference_fps(int fps) {
    std::lock_guard<std::mutex> lk(m_);
    if (fps < 1 || fps > 60) return Result::error();
    cfg_.inference_fps = fps;
    if (state_ == AiState::Active || state_ == AiState::Starting) { stop_locked(); return start_locked(); }
    return Result::ok();
}

void DetectionService::apply_config(const AiConfig& cfg) {
    std::lock_guard<std::mutex> lk(m_);
    bool was = (state_ == AiState::Active || state_ == AiState::Starting);
    cfg_ = cfg;
    if (was) stop_locked();
    if (cfg_.enabled) start_locked();
}

AiTelemetry DetectionService::telemetry() const {
    std::lock_guard<std::mutex> lk(m_);
    AiTelemetry t;
    t.state = state_; t.enabled = cfg_.enabled; t.detector = cfg_.detector; t.backend = backend_;
    t.last_error = last_error_; t.requested_fps = cfg_.inference_fps;
    if (det_) t.skipped = det_->skipped();
    std::lock_guard<std::mutex> tl(tel_m_);
    t.completed = completed_; t.failed = failed_; t.detections_total = detections_;
    t.effective_fps = eff_fps_; t.last_inference_ms = last_inference_ms_; t.last_detection_ms = last_detection_ms_;
    t.last_infer_duration_ms = last_dur_ms_;
    t.avg_infer_duration_ms = dur_n_ > 0 ? dur_sum_ms_ / (double)dur_n_ : 0.0;
    t.motion_now = motion_now_;
    return t;
}

}} // namespace machino::detection
