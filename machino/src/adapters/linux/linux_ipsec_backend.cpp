#include "adapters/linux/linux_ipsec_backend.hpp"

#include <cerrno>
#include <cstring>

#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

namespace machino { namespace ipsec {

LinuxIpsecBackend::LinuxIpsecBackend(std::string init_script, std::string ctl_socket)
    : init_script_(std::move(init_script)), ctl_socket_(std::move(ctl_socket))
{
}

bool LinuxIpsecBackend::ctl_command(const char* cmd, std::string& out)
{
    out.clear();
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return false;

    sockaddr_un a;
    std::memset(&a, 0, sizeof(a));
    a.sun_family = AF_UNIX;
    if (ctl_socket_.size() >= sizeof(a.sun_path)) { ::close(fd); return false; }
    std::memcpy(a.sun_path, ctl_socket_.c_str(), ctl_socket_.size() + 1);

    if (::connect(fd, (sockaddr*)&a, sizeof(a)) != 0) { ::close(fd); return false; }

    const size_t len = std::strlen(cmd);
    size_t off = 0;
    while (off < len) {
        const ssize_t n = ::write(fd, cmd + off, len - off);
        if (n < 0) { if (errno == EINTR) continue; ::close(fd); return false; }
        off += (size_t)n;
    }
    ::shutdown(fd, SHUT_WR);

    char buf[2048];
    ssize_t n;
    while ((n = ::read(fd, buf, sizeof(buf))) > 0) out.append(buf, (size_t)n);
    ::close(fd);
    return true;
}

bool LinuxIpsecBackend::daemon_running()
{
    // "Laeuft" heisst: der Daemon nimmt eine ctl-Verbindung AN. Ein
    // verwaister Socket-File liefert ECONNREFUSED und zaehlt damit korrekt
    // als "laeuft nicht" -- kein pidfile-Raten.
    std::string dummy;
    return ctl_command("status", dummy);
}

bool LinuxIpsecBackend::run_init(const char* verb, std::string& err)
{
    if (::access(init_script_.c_str(), X_OK) != 0) {
        err = init_script_ + " fehlt oder ist nicht ausfuehrbar";
        return false;
    }
    const pid_t pid = ::fork();
    if (pid < 0) { err = "fork fehlgeschlagen"; return false; }
    if (pid == 0) {
        char* const argv[] = {const_cast<char*>(init_script_.c_str()),
                              const_cast<char*>(verb), nullptr};
        ::execv(argv[0], argv);
        _exit(127);
    }
    int st = 0;
    while (::waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
    if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
        err = init_script_ + std::string(" ") + verb + " lieferte Status "
              + std::to_string(WIFEXITED(st) ? WEXITSTATUS(st) : -1);
        return false;
    }
    return true;
}

bool LinuxIpsecBackend::start_daemon(std::string& err)
{
    return run_init("start", err);
}

bool LinuxIpsecBackend::stop_daemon(std::string& err)
{
    // Erst der hoefliche Weg: ctl "down" laesst weirdiked ein RFC-7296-
    // DELETE senden und geordnet enden (Zeroisierung inklusive). Danach
    // raeumt das Init-Skript pidfile/Prozessrest ab; das ist idempotent.
    std::string dummy;
    ctl_command("down", dummy);
    return run_init("stop", err);
}

bool LinuxIpsecBackend::ctl_status(std::string& out)
{
    return ctl_command("status", out);
}

}} // namespace machino::ipsec
