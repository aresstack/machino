// Ethernet as an uplink.
//
// It is the thin one on purpose. Ethernet needs no association, no keys and no
// modem dialogue: the cable is either carrying or it is not, and udhcpc or the
// boot scripts own the address. So this class reports, and refuses to
// configure.
//
// connect() and disconnect() deliberately do NOT bring the interface down.
// eth0 is how this camera is reached; a failover policy that decided to
// "disconnect" it would take the box off the network with no way back. They
// are accepted and reported as unsupported instead, which the manager treats
// as "this uplink manages itself".
#pragma once
#include "adapters/linux/linux_netif.hpp"
#include "ports/inetwork.hpp"
#include <functional>
#include <string>

namespace machino { namespace linuxsys {

class LinuxEthernetUplink : public net::INetworkUplink {
public:
    // A default route through this interface says traffic CAN leave; it does
    // not say anything arrives. Pass a probe to find out for real -- the
    // connectivity manager only ever asks, it never blocks on it, so the probe
    // must be something already-measured rather than a fresh DNS lookup.
    using ReachabilityFn = std::function<bool(const std::string& ifname)>;

    explicit LinuxEthernetUplink(std::string ifname = "eth0",
                                 std::string sys_root = "/sys",
                                 std::string proc_root = "/proc");

    void set_reachability(ReachabilityFn fn) { reach_ = std::move(fn); }

    std::string        id() const override;
    net::UplinkType    type() const override { return net::UplinkType::Ethernet; }
    net::LinkState     state() const override;
    net::NetworkInfo   info() const override;
    net::UplinkMetrics metrics() const override;

    Result connect() override;
    Result disconnect() override;

    bool has_internet() const override;

private:
    LinuxNetif     netif_;
    ReachabilityFn reach_;
};

}} // namespace machino::linuxsys
