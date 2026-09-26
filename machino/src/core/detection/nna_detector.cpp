#include "core/detection/nna_detector.hpp"
#include "core/log.hpp"

#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

namespace machino { namespace detection {

static const char* MOD = "NNA";

static int64_t mono_ms() {
    struct timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

NnaDetector::NnaDetector(INnaProcess& proc, IAnalysisSource& src, NnaDetectorConfig cfg,
                         std::function<int64_t()> now_ms)
    : proc_(proc), src_(src), cfg_(std::move(cfg)),
      now_(now_ms ? std::move(now_ms) : std::function<int64_t()>(mono_ms)) {}

NnaDetector::~NnaDetector() { stop(); }

bool NnaDetector::spawn_helper() {
    last_spawn_ms_ = now_();
    const bool ok = proc_.spawn({cfg_.helper_path,
                                 "--model", cfg_.model_path,
                                 "--width", std::to_string(src_.width()),
                                 "--height", std::to_string(src_.height())});
    if (!ok) {
        // Not installed / not startable. Down with backoff, like a dead child:
        // the operator installing the payload later must not require a restart
        // of machinod, only patience until the next backoff window.
        LOGW(MOD, "helper %s did not start", cfg_.helper_path.c_str());
        helper_ = Helper::Down;
        return false;
    }
    helper_ = Helper::Loading;
    loading_since_ms_ = now_();
    return true;
}

Result NnaDetector::start() {
    if (running_) return Result::ok();
    Result sr = src_.start();
    if (!sr) return sr;
    running_ = true;
    next_due_ms_ = now_();
    // Spawning is cheap; WAITING for "ready" is not (a model load is seconds).
    // start() returns immediately and poll() advances the loading -- the
    // service's start path stays fast and the video pipeline untouched.
    spawn_helper();
    return Result::ok();
}

Result NnaDetector::stop() {
    if (!running_) return Result::ok();
    running_ = false;
    if (helper_ != Helper::Down) {
        proc_.write_line("quit\n");   // polite; terminate() is the guarantee
        proc_.terminate();
        helper_ = Helper::Down;
    }
    src_.stop();
    return Result::ok();
}

// Loading -> Ready, bounded by ready_timeout_ms overall.
Result NnaDetector::advance_loading(int timeout_ms) {
    std::string line;
    if (proc_.read_line(line, timeout_ms)) {
        if (line.rfind("ready", 0) == 0) {
            helper_ = Helper::Ready;
            LOGI(MOD, "helper ready (model %s)", cfg_.model_path.c_str());
            return Result::timeout();   // no result THIS cycle; next poll infers
        }
        // Anything else during load is the helper talking (progress, errors):
        // logged, not parsed. "error ..." during load counts as a failure.
        if (line.rfind("error", 0) == 0) {
            LOGW(MOD, "helper load error: %s", line.c_str());
            proc_.terminate();
            helper_ = Helper::Down;
            return Result::error();
        }
        return Result::timeout();
    }
    if (!proc_.alive()) {
        LOGW(MOD, "helper died while loading");
        helper_ = Helper::Down;
        return Result::error();
    }
    if (now_() - loading_since_ms_ > cfg_.ready_timeout_ms) {
        LOGW(MOD, "helper did not become ready in %d ms", cfg_.ready_timeout_ms);
        proc_.terminate();
        helper_ = Helper::Down;
        return Result::error();
    }
    return Result::timeout();
}

bool NnaDetector::write_frame_file(const AnalysisFrame& f) {
    FILE* fp = ::fopen(cfg_.frame_path.c_str(), "wb");
    if (!fp) return false;
    const size_t n = ::fwrite(f.data, 1, f.size, fp);
    const bool ok = (n == f.size) && (::fclose(fp) == 0);
    if (!ok) ::remove(cfg_.frame_path.c_str());
    return ok;
}

Result NnaDetector::infer_one(detection::DetectionResult& out) {
    AnalysisFrame f;
    Result gr = src_.get(f, 200);
    if (gr.status == Status::Timeout) return Result::timeout();
    if (!gr) return gr;

    const bool sent = write_frame_file(f) &&
        proc_.write_line("frame " + std::to_string(f.pts_us) + " " +
                         std::to_string(f.width) + " " + std::to_string(f.height) + " " +
                         std::to_string(f.stride) + " " + std::to_string(f.size) + " " +
                         cfg_.frame_path + "\n");
    // The IMP buffer goes back BEFORE waiting on the helper: holding it across
    // an inference would starve a 2-buffer analysis channel for no gain -- the
    // bytes are already in the frame file.
    src_.release();
    if (!sent) {
        // A failed send usually IS the death notice -- write_line into a dead
        // child fails before any read would. Without this check the state
        // stayed Ready and every cycle failed loudly instead of entering the
        // quiet backoff (caught by the host test, 2026-09-26).
        if (!proc_.alive()) { helper_ = Helper::Down; LOGW(MOD, "helper gone (send failed)"); }
        return Result::error();
    }

    std::string line;
    if (!proc_.read_line(line, cfg_.result_timeout_ms)) {
        if (!proc_.alive()) { helper_ = Helper::Down; LOGW(MOD, "helper died mid-inference"); }
        return Result::error();
    }
    if (line.rfind("error", 0) == 0) {
        LOGW(MOD, "helper: %s", line.c_str());
        return Result::error();
    }
    int64_t pts = 0; int n = 0;
    if (::sscanf(line.c_str(), "result %" SCNd64 " %d", &pts, &n) != 2 || n < 0 || n > 256)
        return Result::error();
    out.frame_pts_us = pts;
    for (int i = 0; i < n; ++i) {
        if (!proc_.read_line(line, cfg_.result_timeout_ms)) return Result::error();
        Detection d;
        if (parse_det_line(line, d)) out.detections.push_back(d);
        // A malformed det line is skipped, not fatal: the count kept us in sync.
    }
    return Result::ok();
}

Result NnaDetector::poll(detection::DetectionResult& out, int timeout_ms) {
    if (!running_) return Result::busy();

    if (helper_ == Helper::Down) {
        if (last_spawn_ms_ >= 0 && now_() - last_spawn_ms_ < cfg_.restart_backoff_ms)
            return Result::timeout();      // backoff window: quiet, not failing
        return spawn_helper() ? Result::timeout() : Result::error();
    }
    if (helper_ == Helper::Loading)
        return advance_loading(timeout_ms);

    // Ready: pace to inference_fps. Not yet due is a Timeout, like a motion
    // poll with no motion -- the service must not count it as anything.
    const int fps = cfg_.inference_fps > 0 ? cfg_.inference_fps : 5;
    const int64_t period = 1000 / fps;
    const int64_t t = now_();
    if (t < next_due_ms_) return Result::timeout();
    next_due_ms_ = (next_due_ms_ == 0 ? t : next_due_ms_) + period;
    if (next_due_ms_ < t) {
        // Hinter dem Takt: die verpassten Perioden sind Frames, die die Quelle
        // produziert hat und die BEWUSST nicht mehr analysiert werden --
        // newest wins, kein Aufholen im Burst. Genau das ist "skipped".
        skipped_ += (unsigned)((t - next_due_ms_) / period + 1);
        next_due_ms_ = t + period;
    }

    const int64_t t0 = now_();
    Result r = infer_one(out);
    if (r) out.infer_duration_ms = now_() - t0;
    return r;
}

bool parse_det_line(const std::string& line, Detection& out) {
    // det <class_id> <confidence> <x> <y> <w> <h> <label>
    int cls = 0, conf = 0; float x = 0, y = 0, w = 0, h = 0; int at = -1;
    if (::sscanf(line.c_str(), "det %d %d %f %f %f %f %n", &cls, &conf, &x, &y, &w, &h, &at) != 6 || at < 0)
        return false;
    if (conf < 0 || conf > 100) return false;
    if (!(x >= 0.f && y >= 0.f && w > 0.f && h > 0.f && x <= 1.f && y <= 1.f && w <= 1.f && h <= 1.f))
        return false;
    out.class_id = cls;
    out.confidence = conf;
    out.box = Box{x, y, w, h};
    out.label = line.substr((size_t)at);
    while (!out.label.empty() && (out.label.back() == '\n' || out.label.back() == '\r'))
        out.label.pop_back();
    if (out.label.empty()) out.label = "object";
    return true;
}

}} // namespace machino::detection
