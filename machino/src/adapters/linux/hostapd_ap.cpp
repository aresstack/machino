#include "adapters/linux/hostapd_ap.hpp"

#include "core/log.hpp"
#include "core/net/hostapd_conf.hpp"

#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#define MOD "WIFIAP"

namespace machino { namespace linuxsys {

namespace {

bool path_exists(const std::string& p)
{
    struct stat st;
    return ::stat(p.c_str(), &st) == 0;
}

} // namespace

HostapdAp::HostapdAp(std::string ifname, HostapdPaths paths)
    : ifname_(std::move(ifname)), paths_(std::move(paths)),
      ctrl_(paths_.ctrl_dir + "/" + ifname_) {}

bool HostapdAp::have_binary(const char* name) const
{
    for (const std::string& dir : paths_.bin_dirs)
        if (path_exists(dir + "/" + name)) return true;
    return false;
}

bool HostapdAp::running() const
{
    // The control socket exists only while hostapd is up with this interface.
    return ctrl_.available();
}

void HostapdAp::describe(net::WifiCapabilities& caps) const
{
    caps.hostapd_available     = have_binary("hostapd");
    caps.dhcp_server_available = have_binary("udhcpd") || have_binary("dnsmasq");

    // driver_ap is about the RADIO, and without nl80211/iw this image cannot
    // ask it. What we CAN observe is whether hostapd is actually up on this
    // interface, which is stronger evidence than a binary existing: hostapd
    // only opens that socket after the driver accepted AP mode.
    if (running()) caps.driver_ap = true;

    if (!caps.present)                    caps.ap_unavailable_reason = "no WiFi radio is bound on " + ifname_;
    else if (!caps.hostapd_available)     caps.ap_unavailable_reason = "hostapd is not in this image";
    else if (!running())                  caps.ap_unavailable_reason =
        "hostapd is installed but not running; it is started by the init script, not by machino";
    else if (!caps.dhcp_server_available) caps.ap_unavailable_reason =
        "no DHCP server in this image; clients would associate and get no address";
    else                                  caps.ap_unavailable_reason.clear();
}

bool HostapdAp::write_file(const std::string& path, const std::string& text, std::string& err) const
{
    // 0600: hostapd.conf carries wpa_passphrase in clear. It has to -- hostapd
    // reads it -- so the mode is the whole protection.
    const std::string tmp = path + ".tmp";
    const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) { err = "could not write " + path; return false; }

    size_t off = 0;
    bool ok = true;
    while (off < text.size()) {
        const ssize_t n = ::write(fd, text.data() + off, text.size() - off);
        if (n > 0) { off += (size_t)n; continue; }
        if (n < 0 && errno == EINTR) continue;
        ok = false;
        break;
    }
    if (ok && ::fsync(fd) != 0) ok = false;
    ::close(fd);
    if (!ok) { ::unlink(tmp.c_str()); err = "could not write " + path; return false; }

    if (::rename(tmp.c_str(), path.c_str()) != 0) {
        ::unlink(tmp.c_str());
        err = "could not replace " + path;
        return false;
    }
    return true;
}

Result HostapdAp::start(const net::WifiApConfig& cfg, std::string& err)
{
    std::lock_guard<std::mutex> g(m_);

    if (!ctrl_.available()) {
        err = "hostapd is not running; it is started by the init script, not by machino";
        return Result::unsupported();
    }

    // Everything is generated and validated BEFORE anything is written, so a
    // refused request leaves the access point that is running alone.
    std::string conf, dhcp;
    if (!net::hostapd_conf_from(cfg, ifname_, paths_.driver, conf, err)) return Result::error();
    if (cfg.dhcp_server && !net::udhcpd_conf_from(cfg, ifname_, dhcp, err)) return Result::error();

    if (!write_file(paths_.conf_path, conf, err)) return Result::error();
    if (cfg.dhcp_server && !write_file(paths_.dhcp_conf_path, dhcp, err)) return Result::error();

    // DISABLE then ENABLE is hostapd's own reconfigure path: it re-reads the
    // file on ENABLE. RELOAD only picks up a subset of the keys, which is how
    // a changed passphrase silently does not take.
    ctrl_.ok_request("DISABLE", 5000);
    if (!ctrl_.ok_request("ENABLE", 5000)) {
        err = "hostapd refused the new configuration";
        // Deliberately not rolled back: hostapd has already read the file, and
        // writing the previous one back without a successful ENABLE would
        // leave the file and the running state disagreeing. The staged-change
        // transaction above this is what restores a working setup.
        return Result::error();
    }

    // The SSID is logged, the passphrase is not and never will be.
    LOGI(MOD, "access point up on %s (ssid %zu bytes, %s, channel %d)",
         ifname_.c_str(), cfg.ssid.size(),
         net::wifi_security_name(cfg.security), cfg.channel);
    return Result::ok();
}

Result HostapdAp::stop()
{
    std::lock_guard<std::mutex> g(m_);
    if (!ctrl_.available()) return Result::unsupported();
    ctrl_.ok_request("DISABLE", 5000);
    return Result::ok();
}

}} // namespace machino::linuxsys
