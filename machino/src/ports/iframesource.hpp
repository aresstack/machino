// Port: a raw video channel (sensor/ISP output at a given geometry and rate).
#pragma once
#include "core/result.hpp"
#include <cstdint>
#include <vector>

namespace machino {

class IFrameSource {
public:
    virtual ~IFrameSource() = default;
    virtual Result enable()  = 0;
    virtual Result disable() = 0;
    virtual int    channel() const = 0;
    // One copy of the current frame as NV12 (Y plane, then interleaved UV),
    // w x h at the channel's own geometry - what majestic's /image.yuv420
    // serves. Only while the channel is enabled; Unsupported where the
    // platform cannot hand frames out.
    virtual Result snap_nv12(std::vector<uint8_t>& out, int w, int h) { (void)out; (void)w; (void)h; return Result::unsupported(); }
};

} // namespace machino
