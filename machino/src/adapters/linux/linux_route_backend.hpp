// The Linux routing table and /etc/resolv.conf.
//
// READING uses /proc/net/route and core/net/proc_net.cpp, which already parses
// that file, byte order and all, against captured fixtures. A netlink dump
// parser would be a second implementation of the same thing with none of those
// tests behind it.
//
// WRITING uses rtnetlink on a socket, and the reason is not elegance. On this
// camera fork+exec while the IMP pipeline is live is the documented trigger of
// an out-of-memory incident, and the OOM killer takes the media daemon -- the
// process holding /dev/watchdog. `ip route add` is a fork. A socket is not.
//
// Nothing here decides anything. Which route should exist is core/net's
// business; this file makes it so and reports what the kernel said.
#pragma once
#include "ports/iroute_backend.hpp"
#include <string>

namespace machino { namespace linuxsys {

class LinuxRouteBackend : public net::IRouteBackend {
public:
    // The paths are injectable so the read side can be pointed at a fixture
    // tree. The WRITE side talks to the kernel and has no such option, which
    // is why the reconciliation logic lives in core and is tested there.
    // Die Grundlinie liegt unter /var/run, und das ist eine Entscheidung ueber
    // die Lebensdauer: tmpfs ueberlebt einen Daemon-Neustart und stirbt beim
    // Reboot. Genau so soll der Besitz an resolv.conf sich verhalten.
    explicit LinuxRouteBackend(std::string proc_root = "/proc",
                               std::string resolv_path = "/etc/resolv.conf",
                               std::string baseline_path = "/var/run/machino-dns-baseline");

    bool default_routes(std::vector<net::DefaultRoute>& out) const override;

    Result add_default(const std::string& ifname, const std::string& gateway,
                       int metric) override;
    Result del_default(const std::string& ifname, const std::string& gateway,
                       int metric) override;

    Result set_dns(const std::vector<std::string>& servers) override;
    bool   dns(std::vector<std::string>& out) const override;

    bool   dns_baseline(std::vector<std::string>& out) const override;
    Result set_dns_baseline(const std::vector<std::string>& servers) override;
    Result clear_dns_baseline() override;

private:
    Result route_op(int nlmsg_type, int flags, const std::string& ifname,
                    const std::string& gateway, int metric);

    std::string proc_;
    std::string resolv_;
    std::string baseline_;
};

}} // namespace machino::linuxsys
