// One Linux network interface, read-only.
//
// Everything an uplink needs to describe itself -- carrier, speed, address,
// gateway, DNS, byte counters -- comes from sysfs and /proc, so this class is
// shared by the Ethernet uplink and the WiFi station uplink rather than
// written twice. It never configures anything: bringing an interface up is
// udhcpc's or the WiFi adapter's job, and mixing the two here is how a
// "status" call ends up changing the network.
//
// No fork, no popen, no ifconfig. Reading a file is not the reason this rule
// exists -- forking while IMP is live is -- but `ip addr show` would be a fork
// per poll, and this is polled.
#pragma once
#include "core/net/proc_net.hpp"
#include "ports/inetwork.hpp"
#include <string>

namespace machino { namespace linuxsys {

class LinuxNetif {
public:
    // `sys_root` and `proc_root` are injectable so the read paths can be
    // pointed at a fixture tree; on the camera they are /sys and /proc.
    explicit LinuxNetif(std::string ifname,
                        std::string sys_root = "/sys",
                        std::string proc_root = "/proc",
                        std::string resolv_path = "/etc/resolv.conf");

    const std::string& ifname() const { return ifname_; }

    bool exists() const;               // the interface is known to the kernel
    bool admin_up() const;             // IFF_UP, i.e. someone brought it up
    bool carrier() const;              // a cable / an association

    // Ethernet link speed in Mbit/s, 0 when the interface cannot say. Reading
    // /sys/class/net/<if>/speed on a down interface returns EINVAL rather than
    // a number, which is not an error worth reporting.
    int speed_mbit() const;

    // The first IPv4 address, netmask and broadcast, via SIOCGIFADDR. This is
    // an ioctl and not a /proc read because /proc/net/fib_trie is a parsing
    // exercise with no stable format.
    bool ipv4(std::string& addr_out, std::string& netmask_out) const;

    // The default route through THIS interface, if there is one. An interface
    // with an address but no default route through it cannot carry general
    // traffic, which is the difference between "up" and "usable".
    bool default_route(net::DefaultRoute& out) const;

    bool counters(uint64_t& rx_out, uint64_t& tx_out) const;

    // resolv.conf is system-wide, not per interface; it is reported here
    // because that is where the status page expects to read it, and it is the
    // DNS this interface's traffic will actually use.
    std::string dns() const;

    // Assembled view. Fields the interface cannot answer stay empty rather
    // than being filled with a plausible-looking default.
    net::NetworkInfo   info() const;
    net::UplinkMetrics metrics() const;

    // Absent when there is no such interface, Down without carrier, Connected
    // once it carries AND has an address. Carrier without an address is
    // Connecting: on a camera that is a DHCP lease in flight, and calling it
    // Connected would make failover hand traffic to an interface that cannot
    // send any.
    net::LinkState state() const;

private:
    std::string read_file(const std::string& path) const;

    std::string ifname_, sys_, proc_, resolv_;
};

}} // namespace machino::linuxsys
