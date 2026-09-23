#include "adapters/linux/linux_netif.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

namespace machino { namespace linuxsys {

namespace {

std::string trim(const std::string& s)
{
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\n' || s[a] == '\r')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\n' || s[b - 1] == '\r')) --b;
    return s.substr(a, b - a);
}

// One ioctl against a throwaway socket. A long-lived descriptor would be
// nicer, but this is polled at seconds, and a cached fd that survives an
// interface going away is a source of stale answers.
bool ifreq_ioctl(const std::string& ifname, unsigned long req, struct ifreq& ifr)
{
    if (ifname.empty() || ifname.size() >= IFNAMSIZ) return false;
    const int fd = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return false;

    std::memset(&ifr, 0, sizeof(ifr));
    std::snprintf(ifr.ifr_name, IFNAMSIZ, "%s", ifname.c_str());
    const bool ok = (::ioctl(fd, req, &ifr) == 0);
    ::close(fd);
    return ok;
}

} // namespace

LinuxNetif::LinuxNetif(std::string ifname, std::string sys_root, std::string proc_root,
                       std::string resolv_path)
    : ifname_(std::move(ifname)), sys_(std::move(sys_root)),
      proc_(std::move(proc_root)), resolv_(std::move(resolv_path)) {}

std::string LinuxNetif::read_file(const std::string& path) const
{
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return std::string();

    std::string out;
    char buf[4096];
    for (;;) {
        const ssize_t n = ::read(fd, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (n == 0) break;
        out.append(buf, (size_t)n);
        // /proc/net/dev on a box with many interfaces is still small, but an
        // unbounded read of a file that turns out to be a pipe is not a thing
        // to leave open.
        if (out.size() > 256 * 1024) break;
    }
    ::close(fd);
    return out;
}

bool LinuxNetif::exists() const
{
    if (ifname_.empty()) return false;
    struct stat st;
    return ::stat((sys_ + "/class/net/" + ifname_).c_str(), &st) == 0;
}

bool LinuxNetif::admin_up() const
{
    struct ifreq ifr;
    if (!ifreq_ioctl(ifname_, SIOCGIFFLAGS, ifr)) return false;
    return (ifr.ifr_flags & IFF_UP) != 0;
}

bool LinuxNetif::carrier() const
{
    // Reading carrier on an administratively down interface returns EINVAL on
    // this kernel, so an empty string means "no", not "unknown".
    return trim(read_file(sys_ + "/class/net/" + ifname_ + "/carrier")) == "1";
}

int LinuxNetif::speed_mbit() const
{
    const std::string s = trim(read_file(sys_ + "/class/net/" + ifname_ + "/speed"));
    if (s.empty()) return 0;
    const long v = std::strtol(s.c_str(), nullptr, 10);
    // A down interface reports -1 on some drivers rather than failing the
    // read; reporting "-1 Mbit" on the status page is worse than saying
    // nothing.
    return (v > 0 && v < 100000) ? (int)v : 0;
}

bool LinuxNetif::ipv4(std::string& addr_out, std::string& netmask_out) const
{
    struct ifreq ifr;
    if (!ifreq_ioctl(ifname_, SIOCGIFADDR, ifr)) return false;
    if (ifr.ifr_addr.sa_family != AF_INET) return false;

    char buf[INET_ADDRSTRLEN];
    const struct sockaddr_in* sin = (const struct sockaddr_in*)(const void*)&ifr.ifr_addr;
    if (!::inet_ntop(AF_INET, &sin->sin_addr, buf, sizeof(buf))) return false;
    addr_out = buf;

    netmask_out.clear();
    if (ifreq_ioctl(ifname_, SIOCGIFNETMASK, ifr) && ifr.ifr_netmask.sa_family == AF_INET) {
        const struct sockaddr_in* m = (const struct sockaddr_in*)(const void*)&ifr.ifr_netmask;
        if (::inet_ntop(AF_INET, &m->sin_addr, buf, sizeof(buf))) netmask_out = buf;
    }
    return true;
}

bool LinuxNetif::default_route(net::DefaultRoute& out) const
{
    const std::vector<net::DefaultRoute> all =
        net::parse_proc_net_route(read_file(proc_ + "/net/route"));
    bool found = false;
    for (const net::DefaultRoute& r : all) {
        if (r.ifname != ifname_) continue;
        if (!found || r.metric < out.metric) { out = r; found = true; }
    }
    return found;
}

bool LinuxNetif::counters(uint64_t& rx_out, uint64_t& tx_out) const
{
    return net::parse_proc_net_dev(read_file(proc_ + "/net/dev"), ifname_, rx_out, tx_out);
}

std::string LinuxNetif::dns() const
{
    const std::vector<std::string> ns = net::parse_resolv_conf(read_file(resolv_));
    return ns.empty() ? std::string() : ns[0];
}

net::NetworkInfo LinuxNetif::info() const
{
    net::NetworkInfo n;
    n.ifname = ifname_;
    ipv4(n.ipv4, n.netmask);

    net::DefaultRoute r;
    if (default_route(r)) n.gateway = r.gateway;
    n.dns = dns();

    // Whether the address came from DHCP is not something the kernel records,
    // so this is left at its default and owned by whoever configured the
    // interface. Guessing from the lease file would be wrong on a static
    // setup that happens to have one left over.
    return n;
}

net::UplinkMetrics LinuxNetif::metrics() const
{
    net::UplinkMetrics m;
    m.carrier = carrier();
    m.link_mbit = speed_mbit();
    counters(m.rx_bytes, m.tx_bytes);
    return m;
}

net::LinkState LinuxNetif::state() const
{
    if (!exists()) return net::LinkState::Absent;
    if (!carrier()) return net::LinkState::Down;

    std::string a, m;
    if (!ipv4(a, m) || a.empty() || a == "0.0.0.0") return net::LinkState::Connecting;
    return net::LinkState::Connected;
}

}} // namespace machino::linuxsys
