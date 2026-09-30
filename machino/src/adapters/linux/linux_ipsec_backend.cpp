#include "adapters/linux/linux_ipsec_backend.hpp"

#include <cerrno>
#include <cstring>

#include <arpa/inet.h>
#include <net/route.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/ip_icmp.h>
#include <sys/time.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
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

bool LinuxIpsecBackend::rekey(bool ike_sa, std::string& out)
{
    // weirdikectl-Protokoll; der Daemon antwortet "ok: ..." oder "error: ...".
    if (!ctl_command(ike_sa ? "ikerekey" : "rekey", out)) return false;
    return out.compare(0, 3, "ok:") == 0;
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

bool LinuxIpsecBackend::host_store_available()
{
    // AP9: nur nachsehen, was DIESES RootFS mitbringt -- nie herunterladen.
    // Dieselben Kandidaten wie im Daemon (der sie tatsaechlich parst).
    static const char* cand[] = {
        "/etc/ssl/certs/ca-certificates.crt", "/etc/ssl/cert.pem", "/etc/ssl/certs", nullptr };
    for (int i = 0; cand[i]; i++) {
        struct stat st;
        if (::stat(cand[i], &st) == 0 && (S_ISREG(st.st_mode) || S_ISDIR(st.st_mode))) return true;
    }
    return false;
}

// ---- AP12: Test-Ping -------------------------------------------------------

namespace {

uint16_t icmp_checksum(const uint8_t* data, size_t len)
{
    uint32_t sum = 0;
    for (size_t i = 0; i + 1 < len; i += 2) sum += (uint32_t)((data[i] << 8) | data[i + 1]);
    if (len & 1) sum += (uint32_t)(data[len - 1] << 8);
    while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
    return (uint16_t)~sum;
}

double now_ms()
{
    timespec ts;
    ::clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

} // namespace

bool LinuxIpsecBackend::ping(const PingRequest& req, PingResult& out)
{
    out = PingResult{};
    sockaddr_in dst;
    std::memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    if (::inet_pton(AF_INET, req.target.c_str(), &dst.sin_addr) != 1) { out.error = "target kein IPv4-Literal"; return false; }

    const int fd = ::socket(AF_INET, SOCK_RAW | SOCK_CLOEXEC, IPPROTO_ICMP);
    if (fd < 0) { out.error = std::string("ICMP-Socket: ") + strerror(errno); return false; }
    if (!req.ifname.empty() &&
        ::setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, req.ifname.c_str(), (socklen_t)(req.ifname.size() + 1)) != 0) {
        out.error = "Tunnel-Interface " + req.ifname + ": " + strerror(errno);
        ::close(fd);
        return false;
    }

    // Identifier = PID (wie ping), Sequenz 1..count. Der Kernel liefert an
    // einen Raw-Socket JEDES ICMP (auch fremde Echos) -- gefiltert wird hier.
    const uint16_t ident = (uint16_t)(::getpid() & 0xffff);
    uint8_t pkt[64];
    double sum = 0;
    for (int seq = 1; seq <= req.count; ++seq) {
        std::memset(pkt, 0, sizeof(pkt));
        icmphdr* h = reinterpret_cast<icmphdr*>(pkt);
        h->type = ICMP_ECHO;
        h->code = 0;
        h->un.echo.id = htons(ident);
        h->un.echo.sequence = htons((uint16_t)seq);
        for (size_t i = sizeof(icmphdr); i < sizeof(pkt); ++i) pkt[i] = (uint8_t)i;   // wie ping: erkennbare Nutzlast
        h->checksum = 0;
        h->checksum = htons(icmp_checksum(pkt, sizeof(pkt)));

        const double t0 = now_ms();
        if (::sendto(fd, pkt, sizeof(pkt), 0, (sockaddr*)&dst, sizeof(dst)) != (ssize_t)sizeof(pkt)) {
            // ENETUNREACH heisst hier: keine Route ueber DIESES Interface.
            out.error = std::string("senden: ") + strerror(errno);
            ::close(fd);
            return false;
        }
        ++out.sent;

        // Warten bis zur Antwort auf GENAU dieses Echo oder bis zum Timeout.
        for (;;) {
            const double left = (double)req.timeout_ms - (now_ms() - t0);
            if (left <= 0) break;
            timeval tv;
            tv.tv_sec = (time_t)(left / 1000.0);
            tv.tv_usec = (suseconds_t)((left - (double)tv.tv_sec * 1000.0) * 1000.0);
            if (tv.tv_sec == 0 && tv.tv_usec < 1000) tv.tv_usec = 1000;
            ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            uint8_t buf[512];
            sockaddr_in from;
            socklen_t fl = sizeof(from);
            const ssize_t n = ::recvfrom(fd, buf, sizeof(buf), 0, (sockaddr*)&from, &fl);
            if (n < 0) { if (errno == EINTR) continue; break; }   // EAGAIN = Timeout
            const double rtt = now_ms() - t0;
            if (n < (ssize_t)sizeof(iphdr)) continue;
            const iphdr* ip = reinterpret_cast<const iphdr*>(buf);
            const size_t ihl = (size_t)ip->ihl * 4;
            if (ihl < sizeof(iphdr) || (size_t)n < ihl + sizeof(icmphdr)) continue;
            const icmphdr* r = reinterpret_cast<const icmphdr*>(buf + ihl);
            if (r->type == ICMP_ECHOREPLY) {
                if (ntohs(r->un.echo.id) != ident || ntohs(r->un.echo.sequence) != (uint16_t)seq) continue;
                if (from.sin_addr.s_addr != dst.sin_addr.s_addr) continue;
                ++out.received;
                if (out.received == 1 || rtt < out.rtt_min_ms) out.rtt_min_ms = rtt;
                if (rtt > out.rtt_max_ms) out.rtt_max_ms = rtt;
                sum += rtt;
                break;
            }
            if (r->type == ICMP_DEST_UNREACH || r->type == ICMP_TIME_EXCEEDED) {
                // Traegt den Kopf UNSERES Echos: nur dann ist es unsere Antwort.
                const size_t inner = ihl + sizeof(icmphdr);
                if ((size_t)n < inner + sizeof(iphdr) + sizeof(icmphdr)) continue;
                const iphdr* iip = reinterpret_cast<const iphdr*>(buf + inner);
                const size_t iihl = (size_t)iip->ihl * 4;
                if ((size_t)n < inner + iihl + sizeof(icmphdr)) continue;
                const icmphdr* ie = reinterpret_cast<const icmphdr*>(buf + inner + iihl);
                if (ie->type != ICMP_ECHO || ntohs(ie->un.echo.id) != ident) continue;
                char who[INET_ADDRSTRLEN];
                ::inet_ntop(AF_INET, &from.sin_addr, who, sizeof(who));
                out.error = std::string(r->type == ICMP_DEST_UNREACH ? "unreachable" : "TTL abgelaufen")
                          + " (ICMP " + std::to_string((int)r->type) + "/" + std::to_string((int)r->code) + " von " + who + ")";
                break;
            }
        }
    }
    ::close(fd);
    if (out.received > 0) {
        out.rtt_avg_ms = sum / (double)out.received;
        out.error.clear();
        return true;
    }
    if (out.error.empty()) out.error = "keine Antwort (" + std::to_string(req.timeout_ms) + " ms je Echo)";
    return false;
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
