// Port: a source of low-resolution analysis frames (NV12), decoupled from the
// encoder path. The Ingenic adapter backs it with an own FrameSource channel
// (scaled, at the inference cadence) and IMP_FrameSource_GetFrame; host tests
// feed synthetic frames. The detector never learns where the pixels came from.
//
// get()/release() are a strict pair: IMP hands out a buffer it wants back, so
// the caller copies or consumes the frame and releases before the next get().
#pragma once
#include "core/result.hpp"
#include <cstddef>
#include <cstdint>

namespace machino {

struct AnalysisFrame {
    const uint8_t* data = nullptr;   // NV12: Y plane then interleaved UV
    size_t  size = 0;                // total bytes (>= stride*height*3/2)
    int     width = 0, height = 0, stride = 0;
    int64_t pts_us = 0;
};

class IAnalysisSource {
public:
    virtual ~IAnalysisSource() = default;

    virtual Result start() = 0;   // frames begin to flow
    virtual Result stop()  = 0;   // and stop again; start() may follow
    // Next frame, waiting at most timeout_ms. Timeout is normal (paced source).
    virtual Result get(AnalysisFrame& out, int timeout_ms) = 0;
    // Return the buffer from the last successful get(). Required before the next get().
    virtual void   release() = 0;

    virtual int width()  const = 0;
    virtual int height() const = 0;
};

} // namespace machino
