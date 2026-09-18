// Linux implementation of the process statistics port: /proc/self/status
// (VmRSS, Threads) and /proc/self/stat (utime+stime) deltas for CPU %.
#pragma once
#include "core/power/system_stats.hpp"
#include <cstdint>

namespace machino { namespace app {

class LinuxSystemStats final : public power::ISystemStats {
public:
    power::ProcessStats sample() override;
private:
    uint64_t last_ticks_ = 0;
    int64_t  last_ms_ = 0;
    long     ncpu_ = 0, hz_ = 0;
};

}} // namespace machino::app
