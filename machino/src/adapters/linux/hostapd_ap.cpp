#include "adapters/linux/hostapd_ap.hpp"

#include "core/log.hpp"
#include "core/net/hostapd_conf.hpp"
#include "core/net/wpa_parse.hpp"

#include <cerrno>
#include <csignal>
#include <cstdio>
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

    // driver_ap is a question about the RADIO, and the presence of a hostapd
    // binary is no evidence at all -- it is a userspace package. Without an
    // nl80211 query this image cannot ask the driver directly, so the only
    // honest source is OBSERVATION: hostapd running with an enabled BSS on
    // this interface has already had the driver accept AP mode.
    //
    // Anything short of that leaves `known` false, which is a third answer and
    // not a no. See WifiCapabilities for why the two must not be collapsed.
    if (running() && bss_enabled()) {
        caps.driver_ap_known = true;
        caps.driver_ap = true;
    }

    if (!caps.present)                    caps.ap_unavailable_reason = "no WiFi radio is bound on " + ifname_;
    else if (!caps.hostapd_available)     caps.ap_unavailable_reason = "hostapd is not in this image";
    else if (!running())                  caps.ap_unavailable_reason =
        "hostapd is installed but not running; it is started by the init script, not by machino";
    else if (!caps.dhcp_server_available) caps.ap_unavailable_reason =
        "no DHCP server in this image; clients would associate and get no address";
    else if (!caps.driver_ap_known)       caps.ap_unavailable_reason =
        "the driver has not been asked whether it supports access point mode "
        "(no nl80211 query in this build); starting one is allowed but unverified";
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

bool HostapdAp::bss_enabled() const
{
    std::string reply;
    if (!ctrl_.request("STATUS", reply, 2000).is_ok()) return false;
    std::string state;
    // hostapd reports the interface state here; ENABLED is the only value that
    // means the driver accepted AP mode and the BSS is on the air.
    if (!net::wpa_status_field(reply, "state", state)) return false;
    return state == "ENABLED";
}

bool HostapdAp::running_ssid(std::string& out) const
{
    std::string reply;
    if (!ctrl_.request("GET_CONFIG", reply, 2000).is_ok()) return false;
    return net::wpa_status_field(reply, "ssid", out);
}

bool HostapdAp::signal_hup() const
{
    // The pid file is written by the init script that started hostapd. No
    // fork, no exec -- kill(2) on a pid we did not create.
    FILE* f = ::fopen(paths_.pid_path.c_str(), "r");
    if (!f) return false;
    long pid = 0;
    const int n = std::fscanf(f, "%ld", &pid);
    ::fclose(f);
    if (n != 1 || pid <= 1) return false;
    return ::kill((pid_t)pid, SIGHUP) == 0;
}

bool HostapdAp::reload_config()
{
    // RELOAD_CONFIG first: it is the explicit "re-read the file" command and
    // it tells us whether it worked. Older builds answer UNKNOWN COMMAND.
    if (ctrl_.ok_request("RELOAD_CONFIG", 5000)) return true;
    return signal_hup();
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

    // Making hostapd re-read the FILE is a separate operation from enabling
    // the BSS, and an earlier version of this code got that wrong: ENABLE maps
    // to hostapd_enable_iface(), which brings up the configuration hostapd
    // already has in memory. DISABLE+ENABLE would therefore have re-raised the
    // OLD access point -- on a fresh camera, the placeholder SSID from the
    // init script -- while reporting success.
    //
    // Re-reading the file is RELOAD_CONFIG (hostapd_reload_config), with
    // SIGHUP as the fallback for builds whose control interface does not carry
    // it. kill(2) is a signal, not a fork, so it stays within the rule that
    // keeps this daemon from spawning processes while IMP is live.
    if (!reload_config()) {
        err = "hostapd would not re-read its configuration (no RELOAD_CONFIG and no pid to signal)";
        return Result::error();
    }

    // Now bring the BSS up. It may already be enabled, in which case hostapd
    // answers FAIL and that is not an error -- which is why the result is not
    // checked here and the verification below is what decides.
    ctrl_.ok_request("ENABLE", 5000);

    // VERIFY, do not assume. This is the whole reason the sequence above is
    // not simply fired and reported as done: read back what hostapd is really
    // serving and compare it with what was asked for.
    std::string live;
    if (!running_ssid(live)) {
        err = "hostapd did not report a running access point after the reconfiguration";
        return Result::error();
    }
    if (live != cfg.ssid) {
        err = "hostapd is serving '" + live + "', not the requested network; "
              "the configuration was written but did not take effect";
        return Result::error();
    }

    // The SSID is logged, the passphrase is not and never will be.
    LOGI(MOD, "access point up on %s (ssid %zu bytes, %s, channel %d)",
         ifname_.c_str(), cfg.ssid.size(),
         net::wifi_security_name(cfg.security), cfg.channel);

    // Said plainly rather than left to be discovered: the SSID and the key go
    // to hostapd over the control socket and take effect now, but udhcpd is
    // started by the init script and does not re-read its config. A pool that
    // was changed in this call is written and will be used at the next
    // S41hostapd restart -- until then clients get the previous pool.
    if (cfg.dhcp_server)
        LOGI(MOD, "DHCP pool written to %s; a CHANGED pool takes effect at the "
                  "next '/etc/init.d/S41hostapd restart'", paths_.dhcp_conf_path.c_str());
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
