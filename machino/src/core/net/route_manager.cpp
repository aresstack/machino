#include "core/net/route_manager.hpp"

#include <algorithm>

namespace machino { namespace net {

namespace {

bool same_route(const DefaultRoute& a, const RouteIntent& b)
{
    return a.ifname == b.ifname && a.gateway == b.gateway && a.metric == b.metric;
}

bool managed(const RoutePlan& plan, const std::string& ifname)
{
    return std::find(plan.managed_ifnames.begin(), plan.managed_ifnames.end(), ifname)
           != plan.managed_ifnames.end();
}

} // namespace

ReconcileReport RouteManager::reconcile(const RoutePlan& plan)
{
    ReconcileReport rep;

    std::vector<DefaultRoute> actual;
    if (!be_.default_routes(actual)) {
        // Could not read the table. This is the one case where doing nothing
        // is clearly right: every decision below is "what is there that should
        // not be", and without knowing what is there the answer would be
        // "everything".
        rep.read_failed = true;
        rep.error = "the routing table could not be read - nothing was changed";
        return rep;
    }

    // ---- add what is missing, FIRST -------------------------------------
    //
    // Interfaces whose planned route is now known to be in the table. Only
    // those may have their old routes cleaned up afterwards.
    std::vector<std::string> installed;
    for (const RouteIntent& want : plan.routes) {
        bool present = false;
        for (const DefaultRoute& have : actual)
            if (same_route(have, want)) { present = true; break; }

        if (!present) {
            const Result rc = be_.add_default(want.ifname, want.gateway, want.metric);
            if (rc.is_ok()) {
                ++rep.added;
                present = true;
            } else if (rep.error.empty()) {
                rep.error = "the default route through " + want.ifname + " could not be installed";
            }
        }
        if (present) installed.push_back(want.ifname);
    }

    // ---- remove what should not be there, AFTERWARDS ---------------------
    for (const DefaultRoute& have : actual) {
        if (!managed(plan, have.ifname)) continue;   // somebody else's route

        bool wanted = false;
        for (const RouteIntent& want : plan.routes)
            if (same_route(have, want)) { wanted = true; break; }
        if (wanted) continue;

        // The replacement has to be in place before the old one goes.
        //
        // Add-before-delete closes the gap; this closes the failure. If the
        // plan wants a route on this interface and installing it did NOT
        // succeed, the route sitting here is the only way out of the camera --
        // removing it would leave no default route at all and no means to
        // install one. A stale metric is a small problem; no route is not.
        if (plan.find(have.ifname) != nullptr &&
            std::find(installed.begin(), installed.end(), have.ifname) == installed.end())
            continue;

        // A route through one of our interfaces that the plan does not
        // contain. Either the uplink lost its address, or a boot script or a
        // DHCP hook installed it at the metric it happens to prefer. Both are
        // now stale, and leaving them means the kernel and the manager
        // disagree about which uplink carries traffic.
        const Result rc = be_.del_default(have.ifname, have.gateway, have.metric);
        if (rc.is_ok()) {
            ++rep.removed;
        } else if (rep.error.empty()) {
            rep.error = "a stale default route through " + have.ifname + " could not be removed";
        }
    }

    // ---- DNS --------------------------------------------------------------
    //
    // Only when the plan names an owner. An empty list means the active uplink
    // had nothing to say about DNS, and overwriting the file with that would
    // be the quiet way to break every outgoing connection on the camera.
    // Ob machino resolv.conf besitzt, steht NICHT in diesem Objekt.
    //
    // Es stand dort einmal, und das war die Luecke: ein Neustart nur des
    // Daemons -- ein Absturz, ein Upgrade -- nahm die Momentaufnahme mit, und
    // der naechste Rueckfall auf Ethernet hatte nichts mehr zurueckzuschreiben.
    // Der Besitz liegt jetzt beim Backend, in einer Datei unter /var/run: sie
    // ueberlebt den Prozess und stirbt beim Reboot, und genau diese
    // Lebensdauer ist die richtige.
    std::vector<std::string> baseline;
    const bool owned = be_.dns_baseline(baseline);

    if (!plan.dns_owner.empty() && !plan.dns.empty()) {
        std::vector<std::string> current;
        // A failed read is treated as "unknown, so write it": the cost is one
        // redundant write, and the alternative is leaving the wrong resolvers
        // in place because the file could not be parsed.
        const bool known = be_.dns(current);

        // Die Momentaufnahme VOR dem ersten Ueberschreiben, und nur dann. Sie
        // spaeter zu erneuern hiesse, machinos eigene Server aufzuzeichnen --
        // und die Rueckgabe waere ein No-op, also dasselbe wie keine.
        //
        // Auch ein LEERER Eintrag wird geschrieben: er heisst "besitzt, aber
        // was vorher dastand, war nicht lesbar". Ihn wegzulassen hiesse,
        // "besitzt nicht" zu behaupten, und dann gaebe niemand die Datei je
        // wieder her.
        if (!owned) {
            const Result rc = be_.set_dns_baseline(known ? current : std::vector<std::string>{});
            if (!rc.is_ok() && rep.error.empty())
                rep.error = "the previous resolvers could not be recorded - "
                            "they will not be restored automatically";
        }

        if (!known || current != plan.dns) {
            const Result rc = be_.set_dns(plan.dns);
            if (rc.is_ok()) {
                rep.dns_written = true;
            } else if (rep.error.empty()) {
                rep.error = "the resolver configuration could not be written";
            }
        }
    } else if (owned) {
        // Nobody owns DNS any more -- the active uplink cannot name its own
        // servers, or there is no active uplink. Put back what was there
        // before machino took over.
        if (baseline.empty()) {
            // Nothing to restore. Leaving machino's servers is wrong; writing
            // an empty file is worse -- it takes name resolution away from the
            // whole camera. The DHCP hooks rewrite the file on their next
            // lease, so this corrects itself; saying nothing about it would
            // not.
            be_.clear_dns_baseline();
            rep.error = "the previous resolvers are unknown - the file keeps the "
                        "ones the last uplink supplied until a lease renews";
        } else {
            const Result rc = be_.set_dns(baseline);
            if (rc.is_ok()) {
                rep.dns_restored = true;
                // Den Besitz ERST nach dem erfolgreichen Zurueckschreiben
                // abgeben. Andersherum waere die Grundlinie weg und die
                // fremden Server stuenden weiter in der Datei -- ohne dass
                // noch jemand wuesste, dass sie dort nicht hingehoeren.
                be_.clear_dns_baseline();
            } else if (rep.error.empty()) {
                rep.error = "the previous resolver configuration could not be restored";
            }
        }
    }

    return rep;
}

}} // namespace machino::net
