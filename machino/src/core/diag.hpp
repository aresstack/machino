// Lock-free breadcrumbs for the watchdog stall marker (hardlock diagnosis).
//
// When the main loop stops advancing, the watchdog feeder thread wants to
// record WHERE it was stuck before the hardware resets the SoC. The feeder must
// never take a lock the wedged loop might already hold -- otherwise the feeder
// wedges too and no marker is ever written. So every field here is a lock-free
// atomic and the reader only ever loads them.
//
// On MIPS32 (this SoC) a pointer- or int-sized atomic is lock-free; a 64-bit
// one is NOT (it falls back to a lock and fails the cross link), so nothing
// here is 64-bit.
//
// Lives in core/ so both the pipeline manager (core) and the main loop, HTTP
// server and watchdog (app) can write to it without app<-core layering going
// the wrong way.
#pragma once
#include <atomic>

namespace machino { namespace diag {

struct Phases {
    // Where the MAIN loop last was. String literals only: they live in static
    // storage, so the bare pointer the feeder loads stays valid.
    std::atomic<const char*> main{"init"};
    // What the single HTTP thread was doing when it last changed phase.
    std::atomic<const char*> http{"idle"};
    // lifecycle::State as an int, mirrored on every transition (lock-free), so
    // the marker can name the pipeline state without taking the pipeline lock.
    std::atomic<int> pipeline_state{0};
};

// Process-global: one camera, one main loop, one HTTP thread. inline so the one
// instance is shared across translation units without a separate .cpp.
inline Phases& phases() {
    static Phases p;
    return p;
}

// Tiny helpers so a caller can set a phase in one readable line.
inline void set_main(const char* p) { phases().main.store(p, std::memory_order_relaxed); }
inline void set_http(const char* p) { phases().http.store(p, std::memory_order_relaxed); }

}} // namespace machino::diag
