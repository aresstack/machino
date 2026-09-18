// Linux implementation of the grace timer port: a one-shot timerfd that the
// application's epoll loop watches. Expiry is delivered by calling consume()
// from the loop, which forwards to PipelineManager::on_grace_timeout().
#pragma once
#include "core/lifecycle/grace_timer.hpp"

namespace machino { namespace lifecycle { class PipelineManager; } }

namespace machino { namespace app {

class LinuxGraceTimer final : public lifecycle::IGraceTimer {
public:
    LinuxGraceTimer();
    ~LinuxGraceTimer() override;
    int  fd() const { return fd_; }
    void arm(int ms) override;
    void disarm() override;
    // Called by the event loop when fd() is readable; returns true if it expired.
    bool consume();
private:
    int fd_ = -1;
};

}} // namespace machino::app
