#include "adapters/linux/wifi_station_uplink.hpp"

namespace machino { namespace linuxsys {

WifiStationUplink::WifiStationUplink(net::IWifiAdapter& wifi, std::string ifname,
                                     std::string sys_root, std::string proc_root)
    : wifi_(wifi), netif_(std::move(ifname), std::move(sys_root), std::move(proc_root)) {}

std::string WifiStationUplink::id() const { return netif_.ifname(); }

net::LinkState WifiStationUplink::state() const
{
    // In AP mode there is no uplink here at all -- not a broken one. Reporting
    // Down would make the status page cry wolf, and could make a failover
    // policy decide to "fix" the radio by tearing the access point down while
    // someone is using it to reach the setup page.
    if (wifi_.mode() != net::WifiMode::Station) return net::LinkState::Absent;

    const net::LinkState assoc = wifi_.state();
    if (assoc != net::LinkState::Connected) return assoc;

    // Associated. Whether we can send anything is the interface's answer:
    // an association with no lease is Connecting, not Connected.
    const net::LinkState ip = netif_.state();
    return (ip == net::LinkState::Connected) ? net::LinkState::Connected
                                             : net::LinkState::Connecting;
}

net::NetworkInfo WifiStationUplink::info() const { return netif_.info(); }

net::UplinkMetrics WifiStationUplink::metrics() const
{
    net::UplinkMetrics m = netif_.metrics();

    // An Ethernet "speed" is meaningless for a radio, and the driver's own
    // /sys speed file is usually absent or wrong. RSSI is the number that
    // actually tells the user something.
    net::WifiNetwork n;
    if (wifi_.station_status(n)) m.rssi_dbm = n.rssi_dbm;
    return m;
}

Result WifiStationUplink::connect()
{
    // There is nothing to connect TO until a station configuration has been
    // set, and setting one is a staged change that goes through the API, not
    // something failover may do on its own.
    return state() == net::LinkState::Connected ? Result::ok() : Result::unsupported();
}

Result WifiStationUplink::disconnect() { return wifi_.stop(); }

bool WifiStationUplink::has_internet() const
{
    if (state() != net::LinkState::Connected) return false;

    // A camera on a WLAN with no uplink is connected and useless. This is the
    // case the port calls out by name, and the default route is what tells
    // them apart.
    net::DefaultRoute r;
    if (!netif_.default_route(r)) return false;

    if (reach_) return reach_(netif_.ifname());
    return true;
}

}} // namespace machino::linuxsys
