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
};

// Der Besitz an resolv.conf und die Momentaufnahme davor liegen NICHT hier,
// sondern beim Backend.
//
// Sie lagen einmal hier, und das war die Luecke: ein Neustart nur des Daemons
// nahm die Momentaufnahme mit, und der naechste Rueckfall auf Ethernet hatte
// nichts zurueckzuschreiben -- die Kamera loeste Namen weiter ueber ein Modem
// auf, das sie nicht mehr benutzte. Siehe IRouteBackend::dns_baseline.

}} // namespace machino::net
