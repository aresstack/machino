// The real /dev/watchdog, using the same ioctls majestic's strings show it
// used: WDIOC_GETSUPPORT and WDIOC_SETTIMEOUT. Linux only - kept out of the
// host build so the policy in WatchdogService stays testable.
#pragma once
#include "app/watchdog.hpp"
#include <string>

namespace machino {

class LinuxWatchdog : public IWatchdogDevice {
public:
    explicit LinuxWatchdog(std::string path = "/dev/watchdog") : path_(std::move(path)) {}
    ~LinuxWatchdog() override;

    bool        open() override;
    bool        set_timeout(int seconds, int& effective) override;
    bool        feed() override;
    void        close(bool disarm) override;
    std::string identity() const override { return identity_; }

private:
    std::string path_;
    std::string identity_ = "unknown";
    int         fd_ = -1;
};

} // namespace machino
