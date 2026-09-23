// IWifiAdapter over wpa_supplicant's control socket.
//
// Station mode only. Access point mode is hostapd's job and lives in its own
// adapter: they are different daemons with different config files, and the one
// thing they share -- the radio -- is exactly why running both at once needs
// the driver to say it can.
//
// No process is ever started from here. wpa_supplicant is brought up by the
// init script; this talks to the one that is running. That is not tidiness:
// forking while the IMP pipeline is live is the documented trigger of an
// out-of-memory incident on this camera, and a scan is something a user does
// while watching the live view.
//
// Capabilities are answered honestly and separately: what the DRIVER can do,
// and what USERSPACE TOOLING exists on this image. A missing hostapd does not
// make the chip incapable, and a capable chip with no hostapd is still no AP.
#pragma once
#include "adapters/linux/wpa_ctrl.hpp"
#include "ports/inetwork.hpp"
#include <mutex>
#include <string>
#include <vector>

namespace machino { namespace linuxsys {

// At namespace scope, not nested: a nested struct with default member
// initialisers cannot be used as a default ARGUMENT of a member function of
// the class that encloses it -- the enclosing class is still incomplete at
// that point, and the compiler says so.
struct WpaPaths {
    std::string ctrl_dir = "/var/run/wpa_supplicant";
    std::string sys_root = "/sys";
    // Looked up to answer "is the tooling there", never executed.
    std::vector<std::string> bin_dirs{"/usr/sbin", "/sbin", "/usr/bin", "/bin"};
};

class WpaSupplicantWifi : public net::IWifiAdapter {
public:
    using Paths = WpaPaths;

    explicit WpaSupplicantWifi(std::string ifname = "wlan0", WpaPaths paths = WpaPaths());

    net::WifiCapabilities capabilities() const override;
    Result scan(std::vector<net::WifiNetwork>& out) override;

    Result start_station(const net::WifiStationConfig& cfg) override;
    Result start_ap(const net::WifiApConfig& cfg) override;
    Result stop() override;

    net::WifiMode  mode() const override;
    net::LinkState state() const override;
    bool station_status(net::WifiNetwork& out) const override;

private:
    bool have_binary(const char* name) const;
    bool driver_bound() const;
    // SCAN then SCAN_RESULTS. wpa_supplicant answers SCAN with OK immediately
    // and finishes later, so the results of the PREVIOUS scan are what a first
    // call returns -- which is why the API contract says a scan may be served
    // from cache rather than pretending to be synchronous.
    Result do_scan(std::vector<net::WifiNetwork>& out);

    std::string ifname_;
    WpaPaths    paths_;
    mutable WpaCtrl ctrl_;
    mutable std::mutex m_;
};

}} // namespace machino::linuxsys
