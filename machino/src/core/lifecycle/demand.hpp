// Machino core: consumer demand. A DemandHandle is an RAII token: while it
// lives, the media pipeline is needed; when it is destroyed (client thread
// ends, session object dies, scope exits) the demand is released. Nobody
// increments or decrements counters by hand.
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
    void         release();          // idempotent

private:
    friend class PipelineManager;
    DemandHandle(PipelineManager* m, ConsumerType t) : mgr_(m), type_(t) {}
    void steal(DemandHandle& o) { mgr_ = o.mgr_; type_ = o.type_; o.mgr_ = nullptr; }
    PipelineManager* mgr_ = nullptr;
    ConsumerType     type_ = ConsumerType::Manual;
};

}} // namespace machino::lifecycle
