#include "app/linux_grace_timer.hpp"
#include <cstdint>
#include <cstring>
#include <sys/timerfd.h>
#include <unistd.h>

namespace machino { namespace app {

LinuxGraceTimer::LinuxGraceTimer() { fd_ = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK); }
LinuxGraceTimer::~LinuxGraceTimer() { if (fd_ >= 0) close(fd_); }

void LinuxGraceTimer::arm(int ms) {
    struct itimerspec its; memset(&its, 0, sizeof its);
    its.it_value.tv_sec  = ms / 1000;
    its.it_value.tv_nsec = (long)(ms % 1000) * 1000000L;
    if (ms <= 0) its.it_value.tv_nsec = 1;          // "immediately", but still through the loop
    timerfd_settime(fd_, 0, &its, nullptr);
}

void LinuxGraceTimer::disarm() {
    struct itimerspec its; memset(&its, 0, sizeof its);   // all zero = disarm
    timerfd_settime(fd_, 0, &its, nullptr);
    uint64_t x; while (read(fd_, &x, sizeof x) > 0) {}    // drop a pending expiry
}

bool LinuxGraceTimer::consume() {
    uint64_t x = 0; bool fired = false;
    while (read(fd_, &x, sizeof x) > 0) fired = fired || x > 0;
    return fired;
}

}} // namespace machino::app
