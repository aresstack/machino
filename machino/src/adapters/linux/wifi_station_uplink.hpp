// The WiFi radio in station mode, seen as an uplink.
//
// Two sources, because neither alone is the answer:
//
//   the WiFi adapter  says whether we are ASSOCIATED  (wpa_supplicant STATUS)
//   the interface     says whether we are ADDRESSED   (sysfs + ioctl)
//
// A camera that has associated but whose DHCP lease never arrived is exactly
// the state that makes failover useful, and it is invisible if you only ask
// one of the two.
//
// Access point mode is NOT this class. An AP is not an uplink: a camera
// serving its own WLAN so a phone can reach the setup page is working as
// intended and has no internet by design. This uplink reports Absent while
// the adapter is in AP mode rather than claiming a dead link, so failover
// leaves the AP alone instead of tearing it down.
#pragma once
#include "adapters/linux/linux_netif.hpp"
#include "ports/inetwork.hpp"
#include <functional>
#include <string>

namespace machino { namespace linuxsys {

class WifiStationUplink : public net::INetworkUplink {
public:
    using ReachabilityFn = std::function<bool(const std::string& ifname)>;

    // The adapter is borrowed; it outlives this.
    WifiStationUplink(net::IWifiAdapter& wifi, std::string ifname = "wlan0",
                      std::string sys_root = "/sys", std::string proc_root = "/proc");

    void set_reachability(ReachabilityFn fn) { reach_ = std::move(fn); }

    std::string        id() const override;
    net::UplinkType    type() const override { return net::UplinkType::Wifi; }
    net::LinkState     state() const override;
    net::NetworkInfo   info() const override;
    net::UplinkMetrics metrics() const override;

    Result connect() override;
    Result disconnect() override;

    bool has_internet() const override;

private:
    net::IWifiAdapter& wifi_;
    LinuxNetif         netif_;
    ReachabilityFn     reach_;
};

}} // namespace machino::linuxsys
