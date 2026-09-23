#include "adapters/linux/linux_ethernet_uplink.hpp"

namespace machino { namespace linuxsys {

LinuxEthernetUplink::LinuxEthernetUplink(std::string ifname, std::string sys_root,
                                         std::string proc_root)
    : netif_(std::move(ifname), std::move(sys_root), std::move(proc_root)) {}

std::string LinuxEthernetUplink::id() const { return netif_.ifname(); }

net::LinkState     LinuxEthernetUplink::state() const   { return netif_.state(); }
net::NetworkInfo   LinuxEthernetUplink::info() const    { return netif_.info(); }
net::UplinkMetrics LinuxEthernetUplink::metrics() const { return netif_.metrics(); }

Result LinuxEthernetUplink::connect()
{
    // Nothing to do, and saying so is the honest answer. Returning Ok would
    // make a caller believe it had just fixed an unplugged cable.
    return netif_.state() == net::LinkState::Connected ? Result::ok() : Result::unsupported();
}

Result LinuxEthernetUplink::disconnect()
{
    // Refused, not ignored. This interface is how the camera is reached; a
    // policy that took it down would strand the box with no way back in.
    return Result::unsupported();
}

bool LinuxEthernetUplink::has_internet() const
{
    if (netif_.state() != net::LinkState::Connected) return false;

    // Without a default route through this interface, traffic for the outside
    // leaves somewhere else -- so whatever this link is good for, it is not an
    // uplink. A gateway-less default route (point to point) still counts.
    net::DefaultRoute r;
    if (!netif_.default_route(r)) return false;

    // A route says packets CAN leave, not that anything comes back. When a
    // probe is wired, it has the last word.
    if (reach_) return reach_(netif_.ifname());
    return true;
}

}} // namespace machino::linuxsys
