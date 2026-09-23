// Access point mode, over hostapd's control socket.
//
// hostapd speaks the same unix-datagram line protocol wpa_supplicant does, so
// WpaCtrl is reused rather than duplicated.
//
// The part worth explaining is how the daemon is NOT started. hostapd runs
// from the init script, before machinod and therefore before IMP exists; this
// class only writes the config and then reconfigures the running daemon with
// DISABLE / ENABLE. Starting hostapd from here would mean fork+exec while the
// media pipeline is live, which is the documented trigger of an out-of-memory
// incident on this camera. The same reasoning already shaped log_reader (one
// fork at start-up) and the wpa_supplicant client (no wpa_cli).
//
// A consequence worth stating plainly: if hostapd is not running, this adapter
// reports AP mode as unavailable with that as the reason. It does not try to
// fix it. An access point that only works after a reboot is a worse surprise
// than one that says why it is greyed out.
#pragma once
#include "adapters/linux/wpa_ctrl.hpp"
#include "ports/inetwork.hpp"
#include <mutex>
#include <string>

namespace machino { namespace linuxsys {

struct HostapdPaths {
    std::string ctrl_dir  = "/var/run/hostapd";
    std::string conf_path = "/etc/machino/hostapd.conf";
    std::string pid_path  = "/var/run/hostapd.pid";
    std::string dhcp_conf_path = "/etc/machino/udhcpd.conf";
    std::string sys_root  = "/sys";
    std::vector<std::string> bin_dirs{"/usr/sbin", "/sbin", "/usr/bin", "/bin"};
    // hostapd's driver name. Wrong here is the single most common reason it
    // refuses to start, so it is a setting rather than a literal.
    std::string driver = "nl80211";
};

class HostapdAp {
public:
    explicit HostapdAp(std::string ifname = "wlan0", HostapdPaths paths = HostapdPaths());

    // Fills in the AP half of the capability answer: what the tooling can do.
    // The caller merges it with the station adapter's view, because the two
    // halves are genuinely independent -- a chip with no hostapd is still a
    // perfectly good station.
    void describe(net::WifiCapabilities& caps) const;

    // Writes the config, then DISABLE + ENABLE over the control socket.
    // Nothing is written unless the whole configuration validated, so a
    // refused request leaves the previous access point running.
    Result start(const net::WifiApConfig& cfg, std::string& err);

    Result stop();

    bool running() const;

    // hostapd's STATUS reports state=ENABLED once the BSS is actually up. That
    // is the only observation in this build that proves the DRIVER accepted AP
    // mode, so it is what sets driver_ap_known.
    bool bss_enabled() const;

    // The SSID hostapd is really serving, read back with GET_CONFIG. Used to
    // VERIFY a reconfiguration instead of assuming it took.
    bool running_ssid(std::string& out) const;

private:
    bool write_file(const std::string& path, const std::string& text, std::string& err) const;
    bool have_binary(const char* name) const;
    // Makes hostapd re-read its configuration file. Returns false when neither
    // mechanism is available.
    bool reload_config();
    bool signal_hup() const;

    std::string  ifname_;
    HostapdPaths paths_;
    mutable WpaCtrl ctrl_;
    mutable std::mutex m_;
};

}} // namespace machino::linuxsys
