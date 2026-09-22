#include "app/log_reader.hpp"
#include "core/log.hpp"

#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

namespace machino {

static const char* MOD = "LOGRD";

LogReader::~LogReader() { stop(); }

Result LogReader::start() {
    // One logread per daemon, ever. Not "one at a time" - one, full stop.
    if (started_) {
        LOGW(MOD, "start() called twice - refusing; a second logread must never exist");
        return Result::busy();
    }
    started_ = true;

    int fds[2];
    if (pipe(fds) != 0) {
        LOGW(MOD, "pipe failed: %s - /ws/logs will be unavailable", strerror(errno));
        return Result::error(errno);
    }
    const pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]); close(fds[1]);
        LOGW(MOD, "fork failed: %s - /ws/logs will be unavailable", strerror(errno));
        return Result::error(errno);
    }
    if (pid == 0) {                       // child: only async-signal-safe calls
        close(fds[0]);
        dup2(fds[1], STDOUT_FILENO);
        dup2(fds[1], STDERR_FILENO);
        close(fds[1]);
        execl("/sbin/logread", "logread", "-f", (char*)nullptr);
        execlp("logread", "logread", "-f", (char*)nullptr);
        _exit(127);
    }
    close(fds[1]);
    fcntl(fds[0], F_SETFL, O_NONBLOCK);
    fcntl(fds[0], F_SETFD, FD_CLOEXEC);
    fd_ = fds[0];
    pid_ = pid;
    LOGI(MOD, "logread -f started once at boot (pid %d); /ws/logs will never fork again", (int)pid);
    return Result::ok();
}

void LogReader::mark_dead() {
    if (fd_ >= 0) { close(fd_); fd_ = -1; }
}

void LogReader::stop() {
    if (fd_ >= 0) { close(fd_); fd_ = -1; }
    if (pid_ <= 0) return;

    kill(pid_, SIGTERM);
    // Bounded wait rather than a single WNOHANG: the observed child did not
    // die on SIGTERM at all, and a pid that is signalled and then forgotten is
    // how the daemon ended up with a logread it could no longer reap.
    bool reaped = false;
    for (int i = 0; i < 20 && !reaped; ++i) {        // up to ~1 s
        if (waitpid(pid_, nullptr, WNOHANG) == pid_) { reaped = true; break; }
        struct timespec ts = {0, 50 * 1000 * 1000};  // 50 ms
        nanosleep(&ts, nullptr);
    }
    if (!reaped) {
        LOGW(MOD, "logread %d ignored SIGTERM - sending SIGKILL", (int)pid_);
        kill(pid_, SIGKILL);
        // SIGKILL cannot be caught, so this waits rather than polling; without
        // it the process would be left as a zombie at shutdown.
        while (waitpid(pid_, nullptr, 0) < 0 && errno == EINTR) {}
    }
    LOGI(MOD, "logread %d stopped", (int)pid_);
    pid_ = -1;
}

} // namespace machino
