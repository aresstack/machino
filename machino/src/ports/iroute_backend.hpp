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
};

}} // namespace machino::net
