// Port: a hardware video encoder channel producing access units.
#pragma once
#include "core/frame.hpp"
#include "core/result.hpp"

namespace machino {

class IEncoder {
public:
    virtual ~IEncoder() = default;
    virtual Result start() = 0;   // begin receiving pictures
    virtual Result stop()  = 0;
    // Waits up to timeout_ms for one encoded frame. Status::Timeout when none.
    virtual Result fetch(AccessUnit& out, int timeout_ms) = 0;
    virtual void   request_idr() = 0;
    virtual int    channel() const = 0;

    // Live controls (M5). `effective` = value read back after applying.
    // Default: unsupported - the service then classifies the control honestly.
    virtual Result set_bitrate(int kbps, int& effective) { (void)kbps; effective = -1; return Result::unsupported(); }
    virtual Result set_fps(int fps, int& effective)      { (void)fps;  effective = -1; return Result::unsupported(); }
};

} // namespace machino
