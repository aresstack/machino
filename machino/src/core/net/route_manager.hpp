// Making the routing table look like the plan, and nothing more.
//
// The two rules that matter are both about restraint.
//
// ADD BEFORE DELETE. A camera is reached over the network it is about to
// reconfigure. Removing the old default route and then installing the new one
// leaves a window with no route at all, and if the second step fails -- a
// gateway that has gone away, a netlink error -- the window never closes. So
// the new route goes in first and the old one goes out afterwards; the kernel
// copes with two default routes for a few milliseconds, which is exactly what
// metrics are for.
//
// ONLY OUR INTERFACES. A default route through an interface no registered
// uplink owns belongs to somebody else: a VPN, a second NIC, whatever the
// operator set up. Deleting it because it was in the way is how a remote
// camera stops answering, and the person it happens to has no way to find out
// why. Reconciliation is restricted to RoutePlan::managed_ifnames.
#pragma once
#include "core/net/route_plan.hpp"
#include "ports/iroute_backend.hpp"
#include <string>

namespace machino { namespace net {

struct ReconcileReport {
    int  added = 0;
    int  removed = 0;
    bool dns_written = false;
    bool dns_restored = false;    // ownership given back, the old servers are in place
    bool read_failed = false;     // the table could not be read: nothing was touched
    std::string error;            // the first failure, in plain words

    bool changed() const { return added > 0 || removed > 0 || dns_written || dns_restored; }
};

class RouteManager {
public:
    // The backend is borrowed and outlives this.
    explicit RouteManager(IRouteBackend& be) : be_(be) {}

    // Bring the kernel in line with `plan`. Safe to call in a loop: when
    // everything already matches it performs no operation at all, which is
    // what keeps a per-second tick from rewriting the routing table per
    // second.
    ReconcileReport reconcile(const RoutePlan& plan);

private:
    IRouteBackend& be_;

    // What the resolver file said before machino first took it over, and
    // whether it currently holds it.
    //
    // Taking ownership without being able to give it back is the failure this
    // exists to prevent. Cellular becomes active, machino writes the carrier's
    // resolvers, Ethernet comes back -- and Ethernet cannot name its own
    // servers, because on Linux it learns them from the very file machino just
    // overwrote. Without a snapshot there is nothing to go back to, and the
    // camera resolves names through a modem it is no longer using.
    bool                     owns_dns_ = false;
    std::vector<std::string> dns_before_;
};

}} // namespace machino::net
