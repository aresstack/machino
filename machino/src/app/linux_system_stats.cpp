#include "app/linux_system_stats.hpp"
#include <cstdio>
#include <cstring>
#include <ctime>
#include <unistd.h>

namespace machino { namespace app {

static int64_t now_ms() { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000; }

power::ProcessStats LinuxSystemStats::sample() {
    power::ProcessStats s;
    FILE* f = fopen("/proc/self/status", "r");
    if (!f) return s;
    char line[256]; bool rss = false, thr = false;
    while (fgets(line, sizeof line, f)) {
        unsigned long v;
        if (sscanf(line, "VmRSS: %lu", &v) == 1) { s.rss_kb = v; rss = true; }
        else if (sscanf(line, "Threads: %lu", &v) == 1) { s.threads = (int)v; thr = true; }
    }
    fclose(f);
    s.available = rss && thr;
    s.cpu_percent = -1.0;
    if ((f = fopen("/proc/self/stat", "r"))) {
        char buf[1024]; size_t n = fread(buf, 1, sizeof buf - 1, f); buf[n] = 0; fclose(f);
        const char* p = strrchr(buf, ')');                     // skip "pid (comm)"
        if (p) {
            unsigned long ut = 0, st = 0; char state; int i = 0;
            // fields after ')': state ppid pgrp session tty tpgid flags minflt cminflt majflt cmajflt utime stime
            if (sscanf(p + 2, "%c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lu %lu", &state, &ut, &st) == 3) {
                (void)i;
                if (!hz_) { hz_ = sysconf(_SC_CLK_TCK); ncpu_ = sysconf(_SC_NPROCESSORS_ONLN); if (ncpu_ < 1) ncpu_ = 1; if (hz_ < 1) hz_ = 100; }
                uint64_t ticks = ut + st; int64_t t = now_ms();
                if (last_ms_ > 0 && t > last_ms_) {
                    double sec = (double)(t - last_ms_) / 1000.0;
                    s.cpu_percent = 100.0 * (double)(ticks - last_ticks_) / (double)hz_ / sec;   // 100% = one core
                }
                last_ticks_ = ticks; last_ms_ = t;
            }
        }
    }
    return s;
}

}} // namespace machino::app
