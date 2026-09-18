// Machino core: media pipeline lifecycle - the single owner of the platform's
// media resources.
//
//   consumers (RTSP sessions; later snapshots/recording/AI) call acquire()
//   and release(). The first acquire brings the platform up and starts the
//   capture thread; when the last consumer leaves, the pipeline is torn down
//   after a grace period ("no consumer, no pipeline").
//
//   start_pipeline() / stop_pipeline() are the explicit operator controls
//   (always-on mode, SIGUSR2/SIGUSR1, later the power policy): start holds one
//   manual reference, stop drops it and forces an immediate teardown. Both
//   work any number of times within one process.
//
// Teardown is deterministic and in reverse order of bring-up; every vendor
// resource is owned through the RAII port objects.
#pragma once
#include "core/config.hpp"
#include "core/frame.hpp"
#include "core/result.hpp"
#include "core/stream_hub.hpp"
#include "ports/iplatform.hpp"
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>

namespace machino {

class Pipeline {
public:
    Pipeline(IPlatform& platform, const AppConfig& cfg, StreamHub& hub);
    ~Pipeline();
    Pipeline(const Pipeline&) = delete;
    Pipeline& operator=(const Pipeline&) = delete;

    // consumer side
    Result acquire();
    void   release();

    // operator side
    Result start_pipeline();
    void   stop_pipeline();

    // periodic, from the main loop: deferred teardown after the grace period
    void   tick(int64_t now_ms);

    bool   running()   const { return running_; }
    int    consumers() const { return refs_; }
    unsigned frames()  const { return frames_; }

private:
    Result start_locked();
    void   stop_locked();
    void   capture_loop();

    IPlatform&        platform_;
    const AppConfig&  cfg_;
    StreamHub&        hub_;
    AuPool            pool_;

    std::mutex        m_;
    int               refs_          = 0;      // consumer references
    bool              manual_        = false;  // start_pipeline() reference
    bool              running_       = false;
    int64_t           idle_since_ms_ = -1;

    std::unique_ptr<IFrameSource> fs_;
    std::unique_ptr<IEncoder>     enc_;
    bool                          bound_ = false;

    std::thread       thread_;
    std::atomic<bool> quit_{false};
    std::atomic<unsigned> frames_{0};
    uint32_t          seq_ = 0;
};

} // namespace machino
