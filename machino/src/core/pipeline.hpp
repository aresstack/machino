// Machino core: media pipeline lifecycle.
//
//   consumers (RTSP sessions, later snapshots/recording/AI) call acquire()
//   and release(). The first acquire brings the platform up and starts the
//   capture thread; when the last consumer leaves, the pipeline is torn down
//   after a grace period ("no consumer, no pipeline"). `always_on` holds one
//   permanent reference so the stream is up from process start.
//
// Teardown is deterministic and in reverse order of bring-up. All IMP-style
// resources are owned here through unique_ptr RAII of the port objects.
#pragma once
#include "core/config.hpp"
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

    // Registers a consumer. Brings the pipeline up if it is the first one.
    Result acquire();
    // Unregisters a consumer. Arms the grace timer if it was the last one.
    void   release();
    // Called periodically from the main loop; performs the deferred teardown.
    void   tick(int64_t now_ms);
    // Hard stop (process exit): releases everything regardless of consumers.
    void   stop();

    bool   running()   const { return running_; }
    int    consumers() const { return refs_; }

private:
    Result start_locked();
    void   stop_locked();
    void   capture_loop();

    IPlatform&        platform_;
    const AppConfig&  cfg_;
    StreamHub&        hub_;

    std::mutex        m_;
    int               refs_        = 0;
    bool              running_     = false;
    int64_t           idle_since_ms_ = -1;

    std::unique_ptr<IFrameSource> fs_;
    std::unique_ptr<IEncoder>     enc_;
    bool                          bound_ = false;

    std::thread       thread_;
    std::atomic<bool> quit_{false};
    uint32_t          seq_ = 0;
};

} // namespace machino
