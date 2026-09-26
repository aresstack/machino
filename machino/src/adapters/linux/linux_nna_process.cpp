#include "adapters/linux/linux_nna_process.hpp"
#include "core/log.hpp"

#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace machino { namespace linuxsys {

static const char* MOD = "NNAPROC";

LinuxNnaProcess::~LinuxNnaProcess() { terminate(); }

void LinuxNnaProcess::close_fds() {
    if (in_fd_  >= 0) { ::close(in_fd_);  in_fd_  = -1; }
    if (out_fd_ >= 0) { ::close(out_fd_); out_fd_ = -1; }
}

bool LinuxNnaProcess::spawn(const std::vector<std::string>& argv) {
    terminate();                       // hoechstens ein Kind zur Zeit
    if (argv.empty()) return false;
    if (::access(argv[0].c_str(), X_OK) != 0) {
        // "Nicht installiert" ist der haeufigste Fall und verdient eine klare
        // Antwort statt eines fork/exec-Fehlschlags mit totem Kind.
        return false;
    }

    int to_child[2] = {-1, -1}, from_child[2] = {-1, -1};
    if (::pipe(to_child) != 0) return false;
    if (::pipe(from_child) != 0) { ::close(to_child[0]); ::close(to_child[1]); return false; }

    const pid_t pid = ::fork();
    if (pid < 0) {
        ::close(to_child[0]); ::close(to_child[1]);
        ::close(from_child[0]); ::close(from_child[1]);
        return false;
    }
    if (pid == 0) {
        // Kind: Pipes auf stdin/stdout, stderr bleibt (Log des Daemons).
        ::dup2(to_child[0], 0);
        ::dup2(from_child[1], 1);
        ::close(to_child[0]); ::close(to_child[1]);
        ::close(from_child[0]); ::close(from_child[1]);
        std::vector<char*> av;
        av.reserve(argv.size() + 1);
        for (const auto& a : argv) av.push_back(const_cast<char*>(a.c_str()));
        av.push_back(nullptr);
        ::execv(av[0], av.data());
        _exit(127);
    }
    ::close(to_child[0]);
    ::close(from_child[1]);
    pid_   = pid;
    in_fd_  = to_child[1];
    out_fd_ = from_child[0];
    buf_.clear();
    // Ein sterbender Helfer darf uns kein SIGPIPE in den Daemon werfen; der
    // write()-Fehler reicht als Nachricht. main() setzt SIGPIPE ohnehin auf
    // IGN -- das hier dokumentiert nur, worauf sich write_line verlaesst.
    ::fcntl(out_fd_, F_SETFL, ::fcntl(out_fd_, F_GETFL, 0) | O_NONBLOCK);
    LOGI(MOD, "spawned %s (pid %d)", argv[0].c_str(), (int)pid);
    return true;
}

bool LinuxNnaProcess::alive() {
    if (pid_ <= 0) return false;
    int st = 0;
    const pid_t r = ::waitpid(pid_, &st, WNOHANG);
    if (r == pid_) {
        LOGI(MOD, "helper pid %d exited (%d)", (int)pid_, WIFEXITED(st) ? WEXITSTATUS(st) : -1);
        pid_ = -1;
        close_fds();
        return false;
    }
    return r == 0;
}

bool LinuxNnaProcess::write_line(const std::string& line) {
    if (in_fd_ < 0) return false;
    size_t off = 0;
    while (off < line.size()) {
        const ssize_t n = ::write(in_fd_, line.data() + off, line.size() - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        off += (size_t)n;
    }
    return true;
}

bool LinuxNnaProcess::read_line(std::string& out, int timeout_ms) {
    if (out_fd_ < 0) return false;
    const auto take = [&]() -> bool {
        const size_t nl = buf_.find('\n');
        if (nl == std::string::npos) return false;
        out = buf_.substr(0, nl + 1);
        buf_.erase(0, nl + 1);
        return true;
    };
    if (take()) return true;

    struct pollfd pf{}; pf.fd = out_fd_; pf.events = POLLIN;
    int left = timeout_ms < 0 ? 0 : timeout_ms;
    for (;;) {
        const int pr = ::poll(&pf, 1, left);
        if (pr <= 0) return false;                 // Timeout oder Fehler
        char tmp[512];
        const ssize_t n = ::read(out_fd_, tmp, sizeof tmp);
        if (n <= 0) return false;                  // EOF: alive() klaert den Rest
        buf_.append(tmp, (size_t)n);
        if (take()) return true;
        // Teilzeile: weiter warten, aber ohne die Wartezeit neu aufzuziehen --
        // grob genuegt hier, der Aufrufer arbeitet ohnehin mit Budgets.
        left = 10;
    }
}

void LinuxNnaProcess::terminate() {
    if (pid_ > 0) {
        ::kill(pid_, SIGTERM);
        // Kurze Gnadenfrist, dann sicher: ein Helfer, der TERM ignoriert,
        // haengt meist in der NNA-Beschleunigung -- KILL ist dann die Wahrheit.
        for (int i = 0; i < 20; ++i) {
            int st = 0;
            if (::waitpid(pid_, &st, WNOHANG) == pid_) { pid_ = -1; break; }
            ::usleep(25 * 1000);
        }
        if (pid_ > 0) {
            ::kill(pid_, SIGKILL);
            ::waitpid(pid_, nullptr, 0);
            pid_ = -1;
        }
    }
    close_fds();
    buf_.clear();
}

}} // namespace machino::linuxsys
