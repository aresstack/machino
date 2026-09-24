// Port: the system routing table and the resolver configuration.
//
// Two operations that no other part of machino may perform directly. Routing
// is a single shared resource -- there is one default route table, and every
// component that writes it without knowing about the others produces a result
// that depends on timing.
//
// No fork. On this camera fork+exec while the IMP pipeline is live is the
// documented trigger of an out-of-memory incident, and `ip route add` is a
// fork. The Linux implementation therefore speaks rtnetlink on a socket, which
// is a thing a process can do to itself.
#pragma once
#include "core/net/proc_net.hpp"
#include "core/result.hpp"
#include <string>
#include <vector>

namespace machino { namespace net {

class IRouteBackend {
public:
    virtual ~IRouteBackend() = default;

    // Every default route currently in the kernel, in no guaranteed order.
    // False means "could not find out", which is NOT "there are none" -- a
    // reconciliation that confused the two would delete routes it never saw.
    virtual bool default_routes(std::vector<DefaultRoute>& out) const = 0;

    // Add or remove ONE default route, identified by all three of interface,
    // gateway and metric. Two default routes through the same interface at
    // different metrics are a normal thing to have during a switch-over, so
    // the metric is part of the identity and not a detail.
    virtual Result add_default(const std::string& ifname, const std::string& gateway,
                               int metric) = 0;
    virtual Result del_default(const std::string& ifname, const std::string& gateway,
                               int metric) = 0;

    // The resolver configuration, replaced as a whole.
    //
    // An empty list is not accepted as "clear the file": that is how a single
    // uplink with a quiet DHCP server takes name resolution away from the
    // entire system. The caller decides not to call this instead.
    virtual Result set_dns(const std::vector<std::string>& servers) = 0;
    virtual bool   dns(std::vector<std::string>& out) const = 0;

    // What the resolver file said BEFORE machino took it over -- and, by its
    // mere existence, whether machino owns it at all.
    //
    // This has to outlive the daemon, and it must NOT outlive a reboot. Both
    // halves matter:
    //
    //   A machino that is restarted (a crash, an upgrade, a SIGHUP that turns
    //   into a restart) had the snapshot in RAM and lost it. The next failover
    //   back to Ethernet then had nothing to restore, and the camera kept
    //   resolving names through a modem it no longer used. That was a known
    //   hole, written down and not closed -- this is the closing.
    //
    //   After a REBOOT the file must be gone, because resolv.conf is then
    //   whatever the boot scripts made it and machino has taken over nothing.
    //   A baseline that survived would restore a stale snapshot over a fresh
    //   file. /var/run is a tmpfs, which gives exactly that lifetime.
    //
    // An EMPTY baseline is a real answer: "we own it, and what was there
    // before could not be read". It is not the same as no baseline at all.
    virtual bool   dns_baseline(std::vector<std::string>& out) const = 0;
    virtual Result set_dns_baseline(const std::vector<std::string>& servers) = 0;
    virtual Result clear_dns_baseline() = 0;
};

}} // namespace machino::net
