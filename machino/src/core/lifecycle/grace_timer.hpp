// Machino core: one-shot grace timer port. The manager arms it when the last
// consumer leaves and disarms it when demand returns; the platform-specific
// implementation (timerfd on Linux, a fake in tests) delivers the expiry back
// through PipelineManager::on_grace_timeout(). Event-driven - no polling.
#pragma once

namespace machino { namespace lifecycle {

class IGraceTimer {
public:
    virtual ~IGraceTimer() = default;
    virtual void arm(int ms) = 0;     // (re)start one-shot
    virtual void disarm() = 0;        // cancel if pending
};

}} // namespace machino::lifecycle
