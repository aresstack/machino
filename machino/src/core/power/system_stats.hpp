// Port: process/system statistics for telemetry (RSS, threads, CPU). Linux
// implementation reads /proc; tests use a fake. Values may be unavailable.
#pragma once
#include <cstdint>

namespace machino { namespace power {

struct ProcessStats {
    bool     available = false;
    uint64_t rss_kb = 0;
    int      threads = 0;
    double   cpu_percent = 0.0;   // since the previous sample; -1 if not yet measurable
};

class ISystemStats {
public:
    virtual ~ISystemStats() = default;
    virtual ProcessStats sample() = 0;
};

}} // namespace machino::power
