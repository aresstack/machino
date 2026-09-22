#include "app/linux_watchdog.hpp"
#include "core/log.hpp"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <linux/watchdog.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace machino {

static const char* MOD = "WDOG";

LinuxWatchdog::~LinuxWatchdog() { close(true); }

bool LinuxWatchdog::open() {
    if (fd_ >= 0) return true;
    // O_CLOEXEC matters more here than usual: an inherited watchdog fd in a
    // child would keep the device open after this process is gone, and on a
    // driver without nowayout that is the difference between "disarmed on
    // exit" and "resets the camera a few seconds later".
    fd_ = ::open(path_.c_str(), O_WRONLY | O_CLOEXEC);
    if (fd_ < 0) {
        LOGW(MOD, "%s: %s", path_.c_str(), strerror(errno));
        return false;
    }
    struct watchdog_info info;
    memset(&info, 0, sizeof info);
    if (ioctl(fd_, WDIOC_GETSUPPORT, &info) == 0) {
        info.identity[sizeof(info.identity) - 1] = 0;
        identity_ = reinterpret_cast<const char*>(info.identity);
        if (identity_.empty()) identity_ = "unnamed";
    } else {
        LOGW(MOD, "WDIOC_GETSUPPORT: %s", strerror(errno));
    }
    return true;
}

bool LinuxWatchdog::set_timeout(int seconds, int& effective) {
    if (fd_ < 0) return false;
    int v = seconds;
    if (ioctl(fd_, WDIOC_SETTIMEOUT, &v) != 0) {
        LOGW(MOD, "WDIOC_SETTIMEOUT(%d): %s", seconds, strerror(errno));
        return false;
    }
    effective = v;                       // the driver writes back what it took
    int back = 0;
    if (ioctl(fd_, WDIOC_GETTIMEOUT, &back) == 0) effective = back;
    return true;
}

bool LinuxWatchdog::feed() {
    if (fd_ < 0) return false;
    // A plain write of anything other than 'V' is the keepalive. WDIOC_KEEPALIVE
    // would also do, but the write is what every driver supports.
    const char c = 'k';
    return ::write(fd_, &c, 1) == 1;
}

void LinuxWatchdog::close(bool disarm) {
    if (fd_ < 0) return;
    if (disarm) {
        // Magic close. Only honoured when the driver was built WITHOUT
        // nowayout; on this camera a hardware test recorded in streamerctl
        // showed majestic releasing the device on stop with no reset, so it is.
        // If a future kernel sets nowayout this write is simply ignored and the
        // hardware fires - which is why the log says what was attempted rather
        // than claiming the watchdog is off.
        const char v = 'V';
        if (::write(fd_, &v, 1) != 1)
            LOGW(MOD, "magic close refused (%s) - the watchdog may still be armed", strerror(errno));
    }
    ::close(fd_);
    fd_ = -1;
}

} // namespace machino
