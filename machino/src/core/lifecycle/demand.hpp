// Machino core: consumer demand. A DemandHandle is an RAII token: while it
// lives, the media pipeline is needed; when it is destroyed (client thread
// ends, session object dies, scope exits) the demand is released. Nobody
// increments or decrements counters by hand.
//
// M8: demand is per stream unit. Unit 0 is the main H.264 stream, unit 1 the
// substream, unit 2 the JPEG path. Base resources (sensor/ISP) are needed
// while ANY unit has demand; each unit's own encoder exists only while that
// unit has demand (plus a grace period).
#pragma once
#include <cstdint>

namespace machino { namespace lifecycle {

enum class ConsumerType : int { Rtsp = 0, Snapshot, Recording, Ai, HttpStream, Onvif, Manual, COUNT };

inline const char* consumer_name(ConsumerType t) {
    switch (t) {
        case ConsumerType::Rtsp:       return "rtsp";
        case ConsumerType::Snapshot:   return "snapshot";
        case ConsumerType::Recording:  return "recording";
        case ConsumerType::Ai:         return "ai";
        case ConsumerType::HttpStream: return "http";
        case ConsumerType::Onvif:      return "onvif";
        case ConsumerType::Manual:     return "manual";
        case ConsumerType::COUNT:      break;
    }
    return "?";
}

// Stream units. Deliberately not the adapter's channel numbers: the mapping
// from unit to IMP group/channel is the adapter's business alone.
enum : int { UNIT_MAIN = 0, UNIT_SUB = 1, UNIT_JPEG = 2, UNIT_COUNT = 3 };

inline const char* unit_name(int u) {
    switch (u) { case UNIT_MAIN: return "main"; case UNIT_SUB: return "sub"; case UNIT_JPEG: return "jpeg"; }
    return "?";
}

class PipelineManager;

class DemandHandle {
public:
    DemandHandle() = default;
    DemandHandle(const DemandHandle&) = delete;
    DemandHandle& operator=(const DemandHandle&) = delete;
    DemandHandle(DemandHandle&& o) noexcept { steal(o); }
    DemandHandle& operator=(DemandHandle&& o) noexcept { if (this != &o) { release(); steal(o); } return *this; }
    ~DemandHandle() { release(); }

    bool         active() const { return mgr_ != nullptr; }
    ConsumerType type()   const { return type_; }
    int          unit()   const { return unit_; }
    void         release();          // idempotent

private:
    friend class PipelineManager;
    DemandHandle(PipelineManager* m, ConsumerType t, int unit) : mgr_(m), type_(t), unit_(unit) {}
    void steal(DemandHandle& o) { mgr_ = o.mgr_; type_ = o.type_; unit_ = o.unit_; o.mgr_ = nullptr; }
    PipelineManager* mgr_ = nullptr;
    ConsumerType     type_ = ConsumerType::Manual;
    int              unit_ = UNIT_MAIN;
};

}} // namespace machino::lifecycle
