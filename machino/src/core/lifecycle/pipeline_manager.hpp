// Machino core: demand-driven media lifecycle - "no consumer, no pipeline".
//
//   COLD_IDLE --first demand--> STARTING --ok--> ACTIVE
//   ACTIVE --last demand gone--> GRACE_IDLE --demand returns--> ACTIVE
//   GRACE_IDLE --grace timeout--> STOPPING --> COLD_IDLE
//   STARTING --failure (rolled back)--> FAILED --new demand--> STARTING
//
// The PipelineManager is the single owner of the media chain (platform,
// frame source, encoder) and the only place that starts or stops it.
// Transitions are serialised by one mutex that is held only during
// transitions; the frame path (capture thread -> StreamHub) never takes it.
// Consumers hold RAII DemandHandles; the grace shutdown is delivered by an
// event-driven one-shot timer (IGraceTimer), no polling.
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
    unsigned generation  = 0;      // completed successful starts
    unsigned start_count = 0;      // attempted starts
    unsigned stop_count  = 0;
    unsigned failed_count = 0;
    std::string last_error;
    unsigned frames_this_run = 0;
};

class PipelineManager {
public:
    PipelineManager(IPlatform& platform, const EffectiveStream& stream, const LifecycleConfig& cfg,
                    IGraceTimer& timer, StreamHub& hub);
    ~PipelineManager();
    PipelineManager(const PipelineManager&) = delete;
    PipelineManager& operator=(const PipelineManager&) = delete;

    // Registers demand. On success the returned handle is active and the
    // pipeline is ACTIVE. On a start failure the handle is inactive.
    DemandHandle acquire(ConsumerType type, Result* result = nullptr);

    // Delivered by the timer implementation when the grace period expired.
    void on_grace_timeout();

    // Process exit: releases everything regardless of outstanding handles.
    void shutdown();

    State state() const;
    Stats stats() const;

private:
    friend class DemandHandle;
    void release(ConsumerType type);              // called by DemandHandle
    void transition(State to, const char* why);   // m_ held
    bool start_locked();                          // m_ held; returns false on failure (rolled back)
    void stop_locked();                           // m_ held
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
    unsigned         generation_ = 0, start_count_ = 0, stop_count_ = 0, failed_count_ = 0;
    std::string      last_error_;
    bool             shutdown_ = false;

    std::unique_ptr<IFrameSource> fs_;
    std::unique_ptr<IEncoder>     enc_;
    bool                          bound_ = false;
    std::thread       thread_;
    std::atomic<bool> quit_{false};
    std::atomic<unsigned> frames_{0};
    uint32_t          seq_ = 0;
};

}} // namespace machino::lifecycle
