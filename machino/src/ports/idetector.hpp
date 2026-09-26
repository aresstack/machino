// Port: a vendor-neutral detector. Two legitimate input modes (GPT M9 preflight):
//
//   BoundSource - the backend binds directly to a logical FrameSource/ISP
//                 output (Ingenic IMP_IVS). The core never sees raw frames; it
//                 drives lifecycle and polls results.
//   FrameView   - the backend consumes a platform-neutral frame (YUV/RGB) that
//                 the core hands it (model runtimes). Reserved for M9+ NNA work.
//
// A backend advertises its mode so the core does not pretend every detector
// implements infer(FrameView). Missing optional vendor libraries must make a
// backend unavailable, never crash startup.
#pragma once
#include "core/detection/types.hpp"
#include "core/result.hpp"
#include <string>

namespace machino {

enum class DetectorInput : int { BoundSource = 0, FrameView = 1 };

struct DetectorParams {
    std::string detector = "motion";    // backend selector
    int         inference_fps = 5;      // requested analysis cadence
    std::string model_path;             // external model, when a backend needs one
    int         source_width = 0, source_height = 0;   // analysis geometry (0 = adapter default)
};

struct FrameView {
    const uint8_t* y = nullptr;         // luma plane (NV12/I420); analysis is luma-only for motion
    int   width = 0, height = 0, stride = 0;
    int64_t pts_us = 0;
};

class IDetector {
public:
    virtual ~IDetector() = default;

    virtual DetectorInput input_mode() const = 0;
    virtual const char*    backend() const = 0;      // e.g. "imp_ivs_move"

    virtual Result start() = 0;
    virtual Result stop()  = 0;

    // BoundSource: block up to timeout_ms for the next analysed result.
    // Timeout is normal (no activity); it is not an error.
    virtual Result poll(detection::DetectionResult& out, int timeout_ms) { (void)out; (void)timeout_ms; return Result::unsupported(); }

    // FrameView: run inference on one frame now. Only for FrameView backends.
    virtual Result infer(const FrameView& in, detection::DetectionResult& out) { (void)in; (void)out; return Result::unsupported(); }

    // Frames, die das Backend WISSENTLICH ausgelassen hat (Inferenz langsamer
    // als die Quelle; newest wins). 0 bei Backends, die es nicht wissen koennen
    // -- IVS verwirft im Treiber, ohne es zu melden. Monoton seit start().
    virtual unsigned skipped() const { return 0; }
};

} // namespace machino
