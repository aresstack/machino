// The syslog reader behind /ws/logs.
//
// WHY THIS IS ITS OWN OBJECT, AND WHY main() CREATES IT FIRST
//
// On the T40NN, forking while the Ingenic IMP stack is initialised leaves the
// process unable to initialise IMP again after the next pipeline teardown:
// `IMP_System_Init` then returns -1 for good, and the damage survives a daemon
// restart. That was measured, not guessed - see docs/incident-2026-09-22-oom.md.
// The same `/ws/logs` subscription taken while the pipeline is cold is
// harmless, and a child forked then may overlap any number of later starts and
// teardowns without trouble. The fork's *timing* is what matters.
//
// So the daemon forks exactly once, at start-up, before anything can have
// touched IMP - and never again. A `/ws/logs` connect or disconnect is then
// only a subscriber coming and going: no fork, no kill, nothing that can reach
// the media path.
//
// The reader keeps running with no subscribers. That is deliberate: its pipe
// has to be drained or `logread` blocks on a full pipe and stops being a log
// reader at all. With nobody listening the lines are simply discarded.
#pragma once
#include "core/result.hpp"
#include <sys/types.h>

namespace machino {

class LogReader {
public:
    LogReader() = default;
    ~LogReader();

    LogReader(const LogReader&) = delete;
    LogReader& operator=(const LogReader&) = delete;

    // Fork `logread -f` once. MUST be called before any IMP initialisation.
    // Calling it a second time is a no-op that returns Busy, so a second
    // logread can never exist however the call sites change.
    Result start();

    // SIGTERM, a bounded wait, then SIGKILL if it is still there - and always
    // a final reap. The child observed in the field ignored the polite request
    // and outlived the daemon's interest in it for minutes.
    void stop();

    int  fd() const   { return fd_; }
    bool running() const { return fd_ >= 0; }
    pid_t pid() const { return pid_; }

    // The reader has failed and its fd is dead (logread exited, or the pipe
    // broke). The server closes its subscribers; it does NOT fork a
    // replacement, because by then IMP may well be live.
    void mark_dead();

private:
    int   fd_  = -1;
    pid_t pid_ = -1;
    bool  started_ = false;
};

} // namespace machino
