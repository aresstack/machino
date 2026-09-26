#include "adapters/linux/linux_ecm_backend.hpp"

#include "core/cellular/modem_ports.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <sys/socket.h>

namespace machino { namespace linuxsys {

namespace {

std::string slurp_trimmed(const std::string& path)
{
    FILE* f = ::fopen(path.c_str(), "rb");
    if (!f) return std::string();
    char buf[256];
    const size_t n = ::fread(buf, 1, sizeof(buf) - 1, f);
    ::fclose(f);
    buf[n] = 0;
    std::string s(buf);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' ||
                          s.back() == ' '  || s.back() == '\t')) s.pop_back();
    return s;
}

std::string link_basename(const std::string& path)
{
    char buf[1024];
    const ssize_t n = ::readlink(path.c_str(), buf, sizeof(buf) - 1);
    if (n <= 0) return std::string();
    buf[n] = 0;
    const char* slash = std::strrchr(buf, '/');
    return slash ? slash + 1 : buf;
}

} // namespace

bool LinuxEcmBackend::find_interface(EcmInterface& out)
{
    const std::string netdir = sys_root_ + "/class/net";
    DIR* d = ::opendir(netdir.c_str());
    if (!d) return false;

    std::vector<cellular::NetDeviceInfo> devs;
    while (struct dirent* e = ::readdir(d)) {
        const std::string name = e->d_name;
        if (name == "." || name == ".." || name == "lo") continue;

        cellular::NetDeviceInfo info;
        info.name = name;
        const std::string base = netdir + "/" + name;
        info.driver = link_basename(base + "/device/driver");

        // Das USB-Geraet liegt ueber dem Interface. Wie beim seriellen Scanner
        // eine Ebene hoch, und wenn dort kein idVendor steht, noch eine --
        // aber nicht weiter, sonst landet man beim Root-Hub und liest dessen
        // Kennung.
        std::string devpath = base + "/device/..";
        info.vid = slurp_trimmed(devpath + "/idVendor");
        if (info.vid.empty()) {
            devpath += "/..";
            info.vid = slurp_trimmed(devpath + "/idVendor");
        }
        if (!info.vid.empty()) info.pid = slurp_trimmed(devpath + "/idProduct");
        devs.push_back(info);
    }
    ::closedir(d);

    const std::string found = cellular::find_ecm_interface(devs, vid_, pid_);
    if (found.empty()) return false;

    out.name = found;
    const std::string base = netdir + "/" + found;
    out.carrier = slurp_trimmed(base + "/carrier") == "1";
    // operstate ist aussagekraeftiger als flags: "up" heisst betriebsbereit,
    // "unknown" kommt bei manchen USB-Netzgeraeten vor und ist kein Fehler.
    const std::string oper = slurp_trimmed(base + "/operstate");
    out.up = (oper == "up" || oper == "unknown");
    return true;
}

bool LinuxEcmBackend::write_request(const std::string& line)
{
    // Atomar: der Helfer liest diese Datei im Sekundentakt und darf nie ein
    // Fragment sehen.
    const std::string tmp = request_path_ + ".new";
    FILE* f = ::fopen(tmp.c_str(), "wb");
    if (!f) return false;
    const bool ok = ::fwrite(line.data(), 1, line.size(), f) == line.size();
    ::fclose(f);
    if (!ok) { ::remove(tmp.c_str()); return false; }
    if (::rename(tmp.c_str(), request_path_.c_str()) != 0) { ::remove(tmp.c_str()); return false; }
    return true;
}

bool LinuxEcmBackend::set_up(const std::string& ifname, bool up)
{
    // Auch das geht ueber den Helfer: `ip link set` ist ein Programm, und
    // dieser Prozess startet keine.
    return write_request(std::string(up ? "up " : "down ") + ifname + "\n");
}

bool LinuxEcmBackend::dhcp_start(const std::string& ifname)
{
    return write_request("dhcp " + ifname + "\n");
}

bool LinuxEcmBackend::dhcp_stop(const std::string& ifname)
{
    return write_request("stop " + ifname + "\n");
}

bool LinuxEcmBackend::set_address(const std::string& ifname, const LinkAddress& a)
{
    // NIC-Modus: die Adresse kommt vom Modem, nicht per DHCP.
    std::string line = "static " + ifname + " " + a.ipv4 + " " +
                       (a.netmask.empty() ? std::string("255.255.255.0") : a.netmask);
    if (!a.gateway.empty()) line += " " + a.gateway;
    if (!a.dns1.empty())    line += " " + a.dns1;
    if (!a.dns2.empty())    line += " " + a.dns2;
    return write_request(line + "\n");
}

void LinuxEcmBackend::teardown(const std::string& ifname)
{
    write_request("stop " + ifname + "\n");
}

bool LinuxEcmBackend::read_address(const std::string& ifname, LinkAddress& out)
{
    // Der Helfer schreibt, was er konfiguriert hat. Die Adresse aus sysfs zu
    // lesen ginge auch, aber Gateway und DNS stehen dort nicht, und zwei
    // Quellen fuer denselben Zustand driften.
    FILE* f = ::fopen(state_path_.c_str(), "rb");
    if (!f) return false;
    char buf[512];
    const size_t n = ::fread(buf, 1, sizeof(buf) - 1, f);
    ::fclose(f);
    buf[n] = 0;

    // Zeilen "key=value", eine davon "iface=<name>".
    LinkAddress a;
    std::string iface;
    const char* p = buf;
    while (*p) {
        const char* eol = std::strchr(p, '\n');
        const std::string line(p, eol ? (size_t)(eol - p) : std::strlen(p));
        const size_t eq = line.find('=');
        if (eq != std::string::npos) {
            const std::string k = line.substr(0, eq), v = line.substr(eq + 1);
            if      (k == "iface")   iface = v;
            else if (k == "ipv4")    a.ipv4 = v;
            else if (k == "netmask") a.netmask = v;
            else if (k == "gateway") a.gateway = v;
            else if (k == "dns1")    a.dns1 = v;
            else if (k == "dns2")    a.dns2 = v;
            else if (k == "mtu")     a.mtu = (int)::strtol(v.c_str(), nullptr, 10);
        }
        if (!eol) break;
        p = eol + 1;
    }
    // Der Zustand eines ANDEREN Interface ist fuer uns keiner.
    if (iface == ifname && a.has_address()) {
        out = a;
        return true;
    }

    // Fallback: die State-Datei kann waehrend einer Lease-Erneuerung kurz fehlen
    // (deconfig -> bound entfernt und schreibt sie neu). Traegt das Interface
    // aber eine echte IPv4, IST die Verbindung da -- und ecm_link darf sie dann
    // NICHT abreissen. Presence kommt vom Interface (getifaddrs), Gateway/DNS
    // aus der State-Datei; fehlt sie gerade, behaelt ecm_link das zuletzt
    // bekannte Gateway. Genau diese State-Datei-Race war der Teardown-Churn
    // (gemessen 2026-09-26).
    LinkAddress fromiface;
    if (read_iface_ipv4(ifname, fromiface)) {
        out = fromiface;
        return true;
    }
    return false;
}

bool LinuxEcmBackend::read_iface_ipv4(const std::string& ifname, LinkAddress& out) const
{
    struct ifaddrs* ifa = nullptr;
    if (::getifaddrs(&ifa) != 0) return false;
    bool found = false;
    for (struct ifaddrs* p = ifa; p; p = p->ifa_next) {
        if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET) continue;
        if (ifname != p->ifa_name) continue;
        char ip[INET_ADDRSTRLEN] = {0};
        const auto* sin = reinterpret_cast<const struct sockaddr_in*>(p->ifa_addr);
        if (!::inet_ntop(AF_INET, &sin->sin_addr, ip, sizeof ip)) continue;
        out = LinkAddress{};
        out.ipv4 = ip;
        if (p->ifa_netmask) {
            char nm[INET_ADDRSTRLEN] = {0};
            const auto* snm = reinterpret_cast<const struct sockaddr_in*>(p->ifa_netmask);
            if (::inet_ntop(AF_INET, &snm->sin_addr, nm, sizeof nm)) out.netmask = nm;
        }
        found = true;
        break;
    }
    ::freeifaddrs(ifa);
    return found && out.has_address();
}

}} // namespace machino::linuxsys
