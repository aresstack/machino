#include "adapters/linux/linux_ipsec_backend.hpp"

#include <cerrno>
#include <cstring>

#include <arpa/inet.h>
#include <net/route.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
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

bool LinuxIpsecBackend::resolve4(const std::string& host, std::string& ip_out)
{
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* res = nullptr;
    if (::getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0 || !res) return false;
    char buf[INET_ADDRSTRLEN] = {0};
    const sockaddr_in* sin = reinterpret_cast<const sockaddr_in*>(res->ai_addr);
    const char* p = ::inet_ntop(AF_INET, &sin->sin_addr, buf, sizeof(buf));
    ::freeaddrinfo(res);
    if (!p) return false;
    ip_out = buf;
    return true;
}

bool LinuxIpsecBackend::peer_route(const std::string& peer_ip, const std::string& ifname,
                                   const std::string& gateway_ip, bool add, std::string& err)
{
    const int s = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) { err = "socket fehlgeschlagen"; return false; }

    rtentry rt{};
    auto* dst = reinterpret_cast<sockaddr_in*>(&rt.rt_dst);
    dst->sin_family = AF_INET;
    if (::inet_pton(AF_INET, peer_ip.c_str(), &dst->sin_addr) != 1) {
        ::close(s); err = "peer_ip kein IPv4-Literal"; return false;
    }
    auto* msk = reinterpret_cast<sockaddr_in*>(&rt.rt_genmask);
    msk->sin_family = AF_INET;
    msk->sin_addr.s_addr = 0xffffffffu;                 // /32: die Hostroute
    auto* gw = reinterpret_cast<sockaddr_in*>(&rt.rt_gateway);
    gw->sin_family = AF_INET;
    rt.rt_flags = RTF_UP | RTF_HOST;
    if (!gateway_ip.empty()) {
        if (::inet_pton(AF_INET, gateway_ip.c_str(), &gw->sin_addr) != 1) {
            ::close(s); err = "gateway kein IPv4-Literal"; return false;
        }
        rt.rt_flags |= RTF_GATEWAY;
    }
    std::string dev = ifname;                           // ioctl will char*
    rt.rt_dev = dev.empty() ? nullptr : &dev[0];

    int rc = ::ioctl(s, add ? SIOCADDRT : SIOCDELRT, &rt);
    if (rc < 0 && add && errno == EEXIST) rc = 0;       // idempotent
    if (rc < 0 && !add && errno == ESRCH) rc = 0;       // schon weg: ok
    if (rc < 0) err = std::string(add ? "SIOCADDRT: " : "SIOCDELRT: ") + strerror(errno);
    ::close(s);
    return rc == 0;
}

bool LinuxIpsecBackend::add_peer_route(const std::string& peer_ip, const std::string& ifname,
                                       const std::string& gateway_ip, std::string& err)
{
    return peer_route(peer_ip, ifname, gateway_ip, true, err);
}

bool LinuxIpsecBackend::del_peer_route(const std::string& peer_ip, const std::string& ifname,
                                       const std::string& gateway_ip, std::string& err)
{
    return peer_route(peer_ip, ifname, gateway_ip, false, err);
}

}} // namespace machino::ipsec
