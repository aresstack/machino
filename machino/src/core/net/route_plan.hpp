// Which default route wins, and whose DNS the system uses.
//
// WHY THIS EXISTS AS ITS OWN FILE
//
// Before it, three things decided routing independently and none of them knew
// about the others: the boot scripts gave eth0 a default route at metric 0,
// udhcpc-wlan.script gave wlan0 one at 200, udhcpc-cellular.script gave the
// modem one at 300. Those numbers are a hard-coded preference order spread
// across three shell files, and they contradict the one place the user
// actually expresses a preference -- the uplink policy. Setting the policy to
// "cellular first" changed which uplink the status page called active and
// changed nothing at all about where the packets went.
//
// So the decision moves here: one pure function, no files, no sockets, no
// shell. It says what the routing table SHOULD look like; a backend makes it
// so, and the shell scripts keep their metrics only as a bootstrap for the
// window before machino has converged.
//
// WHY THE ACTIVE UPLINK GETS ITS OWN METRIC
//
// Rank alone is not enough. Ethernet can be first in the policy, have a cable,
// have a default route -- and have no internet behind it. The manager then
// correctly selects cellular, and the kernel keeps sending every packet down
// the lower-metric Ethernet route, because the kernel was never told. The
// active uplink therefore gets a metric strictly below every ranked one, so
// that the manager's choice and the kernel's choice are the same choice.
//
// WHY THERE IS NO METRIC 0 HERE
//
// A boot script leaves eth0's default route at metric 0. If the plan used 0
// too, two default routes would tie, and which one the kernel picks then
// depends on insertion order -- which is to say, on the boot sequence. The
// plan uses 10 and the reconciliation removes the metric-0 route it replaces,
// which is the only way "deterministic" means anything.
#pragma once
#include "core/net/connectivity.hpp"
#include "core/net/proc_net.hpp"
#include <string>
#include <vector>

namespace machino { namespace net {

// The metric of the uplink that is actually carrying traffic. Strictly below
// every ranked metric, and not a value a boot script hands out.
const int kActiveRouteMetric = 10;

// Rank n (0-based) in the policy order maps to (n+1) * 100.
const int kRouteMetricStep = 100;

// Not mentioned in the policy at all. Reachable if it is the only thing left,
// never preferred.
const int kUnrankedRouteMetric = 1000;

struct RouteIntent {
    std::string uplink_id;
    std::string ifname;
    std::string gateway;    // "" is legal: a link-local (device) default route
    int         metric = 0;
    bool        active = false;
};

struct RoutePlan {
    // One entry per uplink that is connected and addressed, most preferred
    // first. An uplink with no interface name is not in here -- there is
    // nothing to route through.
    std::vector<RouteIntent> routes;

    // Every interface belonging to a registered uplink, whether or not it got
    // a route. This is the ONLY set reconciliation is allowed to touch: a
    // default route through an interface machino knows nothing about (a VPN, a
    // second NIC someone added, a container bridge) is somebody else's, and
    // deleting it because it was in the way is how a remote camera is lost.
    std::vector<std::string> managed_ifnames;

    // The DNS servers to publish, taken from the ACTIVE uplink alone.
    //
    // Empty with an empty owner means: do not touch resolv.conf. That is not
    // the same as "no DNS". A lease that carried no servers must not wipe the
    // file -- that would take name resolution away from the whole system
    // because one uplink was quiet about it.
    std::vector<std::string> dns;
    std::string              dns_owner;   // uplink id, "" when nobody owns it

    const RouteIntent* find(const std::string& ifname) const;
};

// The intended routing table. Deterministic: same inputs, same output, in a
// defined order.
RoutePlan plan_routes(const std::vector<UplinkStatus>& uplinks,
                      const UplinkPolicy& policy,
                      const std::string& active_id);

// Where an uplink sits in the policy order; -1 when it is not mentioned.
// Matching is the same as the manager's: id first, then type name.
int policy_rank(const UplinkStatus& u, const UplinkPolicy& policy);

// Split a NetworkInfo::dns field ("1.1.1.1 8.8.8.8") into servers, dropping
// anything that is not a dotted quad. A malformed entry in resolv.conf is
// worse than a missing one: the resolver waits for it.
std::vector<std::string> split_dns(const std::string& field);

}} // namespace machino::net
