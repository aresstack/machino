// The person/object detector: machinod's half of the machino-nna helper.
//
// BoundSource on purpose, although the frames pass through this class: the
// DetectionService's poll loop is exactly right for "one result per analysed
// frame, timeouts are quiet", and a second pump thread for FrameView would
// duplicate it. This detector owns the pacing (inference_fps), the frame
// hand-over (file + line protocol, see inna_process.hpp) and the helper's
// lifecycle including restarts -- the service above it only sees results,
// timeouts and the occasional failed cycle.
//
// Protocol (lines to the helper's stdin / from its stdout):
//   -> (spawn) machino-nna --model <path> --width <w> --height <h>
//   <- ready                                   (after the model is loaded)
//   -> frame <pts_us> <w> <h> <stride> <bytes> <path>
//   <- result <pts_us> <n>
//   <- det <class_id> <confidence 0..100> <x> <y> <w> <h> <label>   (n times,
//      box normalised [0,1])
//   <- error <text>                            (instead of result)
//   -> quit                                    (on stop; TERM follows anyway)
//
// The helper dying is an expected event, not a crash of anything that
// matters: the next poll respawns it after a backoff, and until "ready"
// returns, polls report Timeout -- the video pipeline never notices.
#pragma once
#include "core/detection/types.hpp"
#include "ports/ianalysis_source.hpp"
#include "ports/idetector.hpp"
#include "ports/inna_process.hpp"
#include <cstdint>
#include <functional>
#include <string>

namespace machino { namespace detection {

struct NnaDetectorConfig {
    std::string helper_path;         // e.g. /usr/sbin/machino-nna
    std::string model_path;          // quantized Magik model
    std::string frame_path;          // tmpfs file the frames travel through
    int inference_fps = 5;
    int ready_timeout_ms   = 20000;  // model load is seconds, not millis
    int result_timeout_ms  = 2000;   // one inference on the NNA is far below this
    int restart_backoff_ms = 5000;   // pause between helper respawns
};

class NnaDetector : public IDetector {
public:
    // Clock injectable for tests; defaults to CLOCK_MONOTONIC millis.
    NnaDetector(INnaProcess& proc, IAnalysisSource& src, NnaDetectorConfig cfg,
                std::function<int64_t()> now_ms = {});
    ~NnaDetector() override;

    DetectorInput input_mode() const override { return DetectorInput::BoundSource; }
    const char*   backend()    const override { return "venus_nna"; }

    Result start() override;
    Result stop()  override;
    Result poll(detection::DetectionResult& out, int timeout_ms) override;

private:
    enum class Helper { Down, Loading, Ready };

    bool spawn_helper();
    Result advance_loading(int timeout_ms);
    Result infer_one(detection::DetectionResult& out);
    bool write_frame_file(const AnalysisFrame& f);

    INnaProcess&      proc_;
    IAnalysisSource&  src_;
    NnaDetectorConfig cfg_;
    std::function<int64_t()> now_;

    bool    running_ = false;
    Helper  helper_ = Helper::Down;
    int64_t loading_since_ms_ = 0;
    int64_t last_spawn_ms_ = -1;
    int64_t next_due_ms_ = 0;
};

}} // namespace machino::detection

namespace machino { namespace detection {
// Parses one "det ..." line; false on malformed input (caller skips the line).
bool parse_det_line(const std::string& line, Detection& out);
}} // namespace machino::detection
