// Machino core: demand-driven media lifecycle - "no consumer, no pipeline".
//
//   COLD_IDLE --first demand--> STARTING --ok--> ACTIVE
//   ACTIVE --last demand gone--> GRACE_IDLE --demand returns--> ACTIVE
//   GRACE_IDLE --grace timeout--> STOPPING --> COLD_IDLE
//   STARTING --failure (rolled back)--> FAILED --new demand--> STARTING
//
// The PipelineManager is the single owner of the media chain (platform,
// frame source, encoder) and the only place that starts, stops or restarts
// it. Transitions are serialised by one mutex that is held only during
// transitions; the frame path (capture thread -> StreamHub) never takes it.
//
// M5: controlled restart for PIPELINE_RESTART settings (demand preserved),
// live setters executed on the running encoder/platform, and a 1 s
// measurement window (encoded fps, bitrate) derived from frame events.
#pragma once
#include "core/config.hpp"
#include "core/frame.hpp"
#include "core/lifecycle/demand.hpp"
#include "core/lifecycle/grace_timer.hpp"
#include "core/result.hpp"
#include "core/stream_hub.hpp"
#include "ports/iplatform.hpp"
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace machino { namespace lifecycle {

enum class State : int { ColdIdle = 0, Starting, Active, GraceIdle, Stopping, Failed };
const char* state_name(State s);

struct LifecycleConfig {
    int idle_grace_ms   = 5000;
    int poll_timeout_ms = 500;
};

struct Stats {
    State    state = State::ColdIdle;
    int      total_demand = 0;
    int      demand[(int)ConsumerType::COUNT] = {};
    unsigned generation  = 0;
    unsigned start_count = 0;
    unsigned stop_count  = 0;
    unsigned failed_count = 0;
    unsigned restart_count = 0;
    std::string last_error;
    unsigned frames_this_run = 0;
};

// Last completed 1 s window of the capture path.
struct Measurement {
    bool     valid = false;
    double   encoded_fps = 0.0;
    double   bitrate_kbps = 0.0;
    unsigned dropped_frames = 0;    // pool exhaustion drops since start of run
};

class PipelineManager {
public:
    // Notified on every state transition (called with the manager lock held:
    // the listener must only enqueue, never call back into the manager).
    using StateListener = std::function<void(State from, State to)>;

    PipelineManager(IPlatform& platform, const EffectiveStream& stream, const LifecycleConfig& cfg,
                    IGraceTimer& timer, StreamHub& hub);
    void set_state_listener(StateListener l) { std::lock_guard<std::mutex> lk(m_); listener_ = std::move(l); }
    ~PipelineManager();
    PipelineManager(const PipelineManager&) = delete;
    PipelineManager& operator=(const PipelineManager&) = delete;

    DemandHandle acquire(ConsumerType type, Result* result = nullptr);
    void on_grace_timeout();
    void shutdown();

    State state() const;
    Stats stats() const;
    Measurement measurement() const;
    EffectiveStream stream() const;

    // --- M5 controls (executed by the owner; the policy lives in PerformanceService)
    // Replace the stream parameters. Cold: stored for the next start. Running
    // and restart_if_running: orderly stop -> start with the same demand.
    Result update_stream(const EffectiveStream& s, bool restart_if_running, std::string& err);
    // Live setters on the running chain; Busy when the pipeline is not running.
    Result live_bitrate(int kbps, int& effective);
    Result live_encoder_fps(int fps, int& effective);
    Result live_sensor_fps(int fps, int& effective);
    Result read_sensor_fps(int& fps);
    // Sensor fps to (re)apply after every start (-1 = platform default).
    void   set_sensor_fps_target(int fps);

    // --- M7: encoder/latency
    Result live_gop(int frames, int& effective);
    Result live_image(ImageControl c, int value, int& effective);
    Result read_exposure(ExposureReadback& out);
    // Ask the encoder for a key frame (new consumer / reconnect). No-op when cold.
    void   request_idr();
    // Called after every successful start with the manager lock held; used by
    // the ImageService to re-apply image settings. Must not call back in.
    using PostStartHook = std::function<void()>;
    void   set_post_start_hook(PostStartHook h) { std::lock_guard<std::mutex> lk(m_); post_start_ = std::move(h); }

private:
    friend class DemandHandle;
    void release(ConsumerType type);
    void transition(State to, const char* why);
    bool start_locked();
    void stop_locked();
    void capture_loop();

    IPlatform&       platform_;
    EffectiveStream  stream_;
    LifecycleConfig  cfg_;
    IGraceTimer&     timer_;
    StreamHub&       hub_;
    AuPool           pool_;

    mutable std::mutex m_;
    State            state_ = State::ColdIdle;
    int              demand_[(int)ConsumerType::COUNT] = {};
    int              total_ = 0;
    unsigned         generation_ = 0, start_count_ = 0, stop_count_ = 0, failed_count_ = 0, restart_count_ = 0;
    std::string      last_error_;
    bool             shutdown_ = false;
    int              sensor_fps_target_ = -1;
    StateListener    listener_;
    PostStartHook    post_start_;

    std::unique_ptr<IFrameSource> fs_;
    std::unique_ptr<IEncoder>     enc_;
    bool                          bound_ = false;
    std::thread       thread_;
    std::atomic<bool> quit_{false};
    std::atomic<unsigned> frames_{0};
    std::atomic<unsigned> dropped_{0};
    uint32_t          seq_ = 0;

    // measurement window (written by the capture thread, read under win_m_)
    mutable std::mutex win_m_;
    Measurement       last_win_;
    int64_t           win_start_us_ = 0;
    unsigned          win_frames_ = 0;
    uint64_t          win_bytes_ = 0;
};

}} // namespace machino::lifecycle
