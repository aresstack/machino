// Machino core: detection/AI service. A detector is a CONSUMER, never a second
// media owner: it holds a base-only demand (sensor/ISP up, no encoder) and runs
// a bound-source or frame-driven backend. Inference cadence is independent of
// video fps. Detector failure degrades AI only - it never touches video or the
// daemon. Events are emitted on detections and on motion on/off transitions,
// not once per analysed frame.
#pragma once
#include "core/config.hpp"
#include "core/detection/types.hpp"
#include "core/events.hpp"
#include "core/lifecycle/demand.hpp"
#include "core/lifecycle/pipeline_manager.hpp"
#include "core/result.hpp"
#include "ports/iplatform.hpp"
#include <atomic>
#include <mutex>
#include <string>
#include <thread>

namespace machino { namespace detection {

enum class AiState : int { Disabled = 0, Starting, Active, Error };
const char* ai_state_name(AiState s);

struct AiTelemetry {
    AiState     state = AiState::Disabled;
    bool        enabled = false;
    std::string detector;              // requested backend name
    std::string backend;              // actual backend id once started ("" otherwise)
    std::string last_error;
    int         requested_fps = 0;
    unsigned    completed = 0;         // analysed results consumed
    unsigned    skipped = 0;           // frames dropped by the latest-frame slot (frame-driven)
    unsigned    failed = 0;            // poll/inference failures
    unsigned    detections_total = 0;  // frames that carried a detection
    double      effective_fps = 0.0;
    int64_t     last_inference_ms = -1;   // realtime epoch ms
    int64_t     last_detection_ms = -1;
    bool        motion_now = false;
};

class DetectionService {
public:
    DetectionService(lifecycle::PipelineManager& pipeline, IPlatform& platform, EventBus& bus, const AiConfig& cfg);
    ~DetectionService();
    DetectionService(const DetectionService&) = delete;
    DetectionService& operator=(const DetectionService&) = delete;

    // Control. Applying settings while active restarts the detector cleanly.
    Result set_enabled(bool on);
    Result set_detector(const std::string& name);
    Result set_inference_fps(int fps);
    void   apply_config(const AiConfig& cfg);

    AiState     state() const;
    AiTelemetry telemetry() const;
    void        shutdown();

private:
    Result start_locked();
    void   stop_locked();
    void   run();                      // bound-source poll loop

    lifecycle::PipelineManager& pipeline_;
    IPlatform&      platform_;
    EventBus&       bus_;

    mutable std::mutex m_;
    AiConfig        cfg_;
    AiState         state_ = AiState::Disabled;
    std::string     backend_, last_error_;
    lifecycle::DemandHandle demand_;
    std::unique_ptr<IDetector> det_;
    std::thread     thread_;
    std::atomic<bool> quit_{false};

    // telemetry (updated by the poll thread under tel_m_)
    mutable std::mutex tel_m_;
    unsigned completed_ = 0, failed_ = 0, detections_ = 0;
    int64_t  last_inference_ms_ = -1, last_detection_ms_ = -1;
    int64_t  win_start_ms_ = 0; unsigned win_completed_ = 0; double eff_fps_ = 0.0;
    bool     motion_now_ = false;
};

}} // namespace machino::detection
