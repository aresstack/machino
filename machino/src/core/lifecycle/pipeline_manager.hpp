// Machino core: demand-driven media lifecycle - "no consumer, no pipeline".
//
//   COLD_IDLE --first demand--> STARTING --ok--> ACTIVE
//   ACTIVE --last demand gone--> GRACE_IDLE --demand returns--> ACTIVE
//   GRACE_IDLE --grace timeout--> STOPPING --> COLD_IDLE
//   STARTING --failure (rolled back)--> FAILED --new demand--> STARTING
//
// M8 splits ownership in two layers that this class still owns alone:
//
//   base            sensor + ISP (platform bring_up/tear_down). Needed while
//                   ANY unit has demand; the state machine above describes it.
//   units           per-stream framesource + encoder + capture thread
//                   (UNIT_MAIN, UNIT_SUB) and the ephemeral JPEG encoder
//                   (UNIT_JPEG). A unit runs only while it has demand; when
//                   its demand ends while the base stays active, a per-unit
//                   grace timer stops just that unit. When ALL demand ends the
//                   base enters GRACE_IDLE with every started unit kept warm,
//                   exactly as before M8 - the single-stream behaviour is
//                   unchanged when only the main stream is used.
//
// Transitions are serialised by one mutex held only during transitions; the
// frame paths (capture threads -> StreamHub) never take it.
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
#include <vector>

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
    int      demand[(int)ConsumerType::COUNT] = {};          // main-unit demand (compat)
    int      unit_demand[UNIT_COUNT] = {};
    bool     unit_active[UNIT_COUNT] = {};
    unsigned generation  = 0;
    unsigned start_count = 0;
    unsigned stop_count  = 0;
    unsigned failed_count = 0;
    unsigned restart_count = 0;
    unsigned sub_restart_count = 0;
    std::string last_error;
    unsigned frames_this_run = 0;                            // main unit
    uint64_t total_bytes[UNIT_COUNT] = {};                   // monotonic encoded bytes per unit
    unsigned jpeg_captures = 0, jpeg_failures = 0;
    int64_t  jpeg_last_capture_ms = -1;                      // duration of the last capture
};

// Last completed 1 s window of a capture path.
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

    // M8: give the manager the substream / JPEG configuration. Both optional;
    // without them the manager behaves exactly like the single-stream M4 one.
    // Timers may be nullptr: the unit then stops immediately when its last
    // demand goes while the base stays active.
    void configure_sub(const EffectiveStream& s, StreamHub& hub, IGraceTimer* timer);
    void configure_jpeg(const JpegParams& p, int cache_ms, int grace_ms, IGraceTimer* timer);
    // Per-unit grace timer (main included, so ch0 also winds down on its own
    // when another consumer keeps the base up). nullptr = stop immediately.
    void set_unit_grace_timer(int unit, IGraceTimer* timer);

    DemandHandle acquire(ConsumerType type, Result* result = nullptr);              // main unit
    DemandHandle acquire_unit(int unit, ConsumerType type, Result* result = nullptr);
    // Base-only demand (UNIT_AI): guarantees sensor/ISP are up without starting
    // any encoder - the minimum a detector needs. Released like any handle.
    DemandHandle acquire_base(ConsumerType type, Result* result = nullptr);

    // One current frame as JPEG. Takes snapshot demand internally: wakes the
    // base from COLD_IDLE if needed, reuses a running pipeline untouched, and
    // releases the demand when done (the encoder stays warm for grace_ms).
    // Requests within cache_ms share one capture instead of hammering the
    // hardware. Never touches the H.264 units.
    Result snapshot(std::vector<uint8_t>& out, std::string& err, int timeout_ms = 5000);

    void on_grace_timeout();               // base grace
    void on_unit_grace(int unit);          // per-unit grace (sub)
    void on_jpeg_grace();
    void shutdown();

    State state() const;
    Stats stats() const;
    Measurement measurement() const;                 // main unit (compat)
    Measurement measurement_unit(int unit) const;
    EffectiveStream stream() const;                  // main unit (compat)
    EffectiveStream stream_unit(int unit) const;
    bool unit_configured(int unit) const;
    bool unit_active(int unit) const;

    // --- M5 controls (executed by the owner; the policy lives in PerformanceService)
    // Replace the stream parameters. Cold: stored for the next start. Running
    // and restart_if_running: orderly stop -> start with the same demand.
    Result update_stream(const EffectiveStream& s, bool restart_if_running, std::string& err);
    // M8: same for the substream; enabled=false stops and removes it.
    Result update_sub_stream(const EffectiveStream& s, bool enabled, std::string& err);
    // Live setters on the running chain; Busy when the pipeline is not running.
    Result live_bitrate(int kbps, int& effective);
    Result live_encoder_fps(int fps, int& effective);
    Result live_sensor_fps(int fps, int& effective);
    Result read_sensor_fps(int& fps);
    // Sensor fps to (re)apply after every start (-1 = platform default).
    void   set_sensor_fps_target(int fps);

    // --- M7: encoder/latency (main unit)
    Result live_gop(int frames, int& effective);
    Result live_image(ImageControl c, int value, int& effective);
    Result read_exposure(ExposureReadback& out);
    // Ask the encoder for a key frame (new consumer / reconnect). No-op when cold.
    void   request_idr(int unit = UNIT_MAIN);
    // Called after every successful base start with the manager lock held; used
    // by the ImageService to re-apply image settings. Must not call back in.
    using PostStartHook = std::function<void()>;
    void   set_post_start_hook(PostStartHook h) { std::lock_guard<std::mutex> lk(m_); post_start_ = std::move(h); }

private:
    friend class DemandHandle;

    struct Unit {
        EffectiveStream stream{};
        bool            configured = false;
        int             demand[(int)ConsumerType::COUNT] = {};
        int             total = 0;
        std::unique_ptr<IFrameSource> fs;
        std::unique_ptr<IEncoder>     enc;
        bool            bound = false;
        bool            running = false;
        std::thread     thread;
        std::atomic<bool> quit{false};
        StreamHub*      hub = nullptr;
        IGraceTimer*    timer = nullptr;
        bool            grace_armed = false;
        std::unique_ptr<AuPool> pool;
        uint32_t        seq = 0;
        std::atomic<unsigned> frames{0};
        std::atomic<unsigned> dropped{0};
        std::atomic<uint64_t> total_bytes{0};   // monotonic encoded bytes (metrics)
        // 1 s measurement window (written by the capture thread)
        mutable std::mutex win_m;
        Measurement     last_win;
        int64_t         win_start_us = 0;
        unsigned        win_frames = 0;
        uint64_t        win_bytes = 0;
    };

    struct JpegUnit {
        bool            configured = false;
        JpegParams      params{};
        int             cache_ms = 300;
        int             grace_ms = 2000;
        int             demand = 0;
        std::unique_ptr<IJpegEncoder> enc;
        IGraceTimer*    timer = nullptr;
        bool            grace_armed = false;
        unsigned        captures = 0, failures = 0;
        int64_t         last_capture_ms = -1;
        std::vector<uint8_t> cache;
        int64_t         cache_at_us = -1;
    };

    void release(ConsumerType type, int unit);
    void transition(State to, const char* why);
    int  total_all_locked() const;
    bool ensure_base_locked(ConsumerType type, int unit);    // ColdIdle/Failed -> Active
    bool start_base_locked(int first_unit);
    void stop_base_locked();
    bool start_unit_locked(int unit);
    void stop_unit_locked(int unit);
    bool start_jpeg_locked();
    void stop_jpeg_locked();
    void capture_loop(Unit& u);

    IPlatform&       platform_;
    LifecycleConfig  cfg_;
    IGraceTimer&     timer_;                 // base grace
    Unit             units_[2];              // UNIT_MAIN, UNIT_SUB
    JpegUnit         jpeg_;
    std::mutex       snap_m_;                // serialises snapshot captures

    mutable std::mutex m_;
    State            state_ = State::ColdIdle;
    unsigned         generation_ = 0, start_count_ = 0, stop_count_ = 0, failed_count_ = 0;
    unsigned         restart_count_ = 0, sub_restart_count_ = 0;
    int              ai_demand_ = 0;              // base-only holders (detectors)
    std::string      last_error_;
    bool             shutdown_ = false;
    int              sensor_fps_target_ = -1;
    StateListener    listener_;
    PostStartHook    post_start_;
};

}} // namespace machino::lifecycle
