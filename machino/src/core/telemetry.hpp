// Machino core: telemetry snapshot (internal in M5; the M6 API exposes it).
// Every value that may legitimately be missing on a platform is an
// Optional: "unavailable" is explicit, never a misleading 0.
#pragma once
#include "core/lifecycle/pipeline_manager.hpp"
#include "core/power/apply.hpp"
#include <cstdint>
#include <string>

namespace machino {

template <typename T>
struct Optional {
    bool available = false;
    T    value{};
    static Optional of(T v) { Optional o; o.available = true; o.value = v; return o; }
    static Optional none() { return Optional{}; }
};

struct Telemetry {
    lifecycle::State state = lifecycle::State::ColdIdle;
    unsigned         generation = 0;
    power::Profile   profile = power::Profile::Performance;

    int requested_sensor_fps = 0;
    Optional<int> effective_sensor_fps;       // read back from the platform (ISP), if readable
    bool          sensor_fps_readback = false; // true = effective value comes from the hardware, not assumed

    int requested_stream_fps = 0;
    Optional<double> measured_encoded_fps;    // from the capture path (1 s window)
    int requested_bitrate_kbps = 0;
    Optional<double> measured_bitrate_kbps;   // from the capture path (1 s window)
    unsigned dropped_frames = 0;

    Optional<double>   cpu_percent;           // process CPU (all cores = 100 * ncpu)
    Optional<uint64_t> rss_kb;
    Optional<int>      threads;

    Optional<uint64_t> isp_clock_hz;          // known (read-only) clocks
    Optional<uint64_t> encoder_clock_hz;
    Optional<uint64_t> cpu_freq_khz;
};

} // namespace machino
