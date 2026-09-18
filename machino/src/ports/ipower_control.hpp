// Port: platform power / performance control. The core policy never sees a
// register or a clock address; the adapter reports honestly what it can read
// and what it can safely change. "unsupported" is a valid answer.
#pragma once
#include "core/capabilities.hpp"
#include "core/power/apply.hpp"
#include <cstdint>
#include <string>

namespace machino {

struct ClockReading {
    bool     available = false;
    uint64_t hz = 0;
};

struct PowerState {
    ClockReading isp_clock;        // e.g. tx-isp core clock
    ClockReading encoder_clock;    // e.g. video encoder (AVPU/EL150) clock
    ClockReading cpu_clock;        // Hz
    std::string  cpu_governor;     // "" if none
};

class IPowerControl {
public:
    virtual ~IPowerControl() = default;
    // Fills isp.performance / encoder.performance / power.cpu_frequency (+ the
    // legacy *_control tri-states) - only what the adapter has established.
    virtual void fill_capabilities(CapabilitySet& caps) const = 0;
    virtual PowerState current_state() = 0;
    virtual power::ApplyResult set_isp_performance(power::PerfLevel level) = 0;
    virtual power::ApplyResult set_encoder_performance(power::PerfLevel level) = 0;
    virtual power::ApplyResult set_cpu_performance(power::PerfLevel level) = 0;
};

} // namespace machino
