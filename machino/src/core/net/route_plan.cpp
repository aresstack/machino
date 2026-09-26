#include "core/net/route_plan.hpp"

#include <algorithm>

namespace machino { namespace net {

namespace {

bool is_dotted_quad(const std::string& s)
{
    int octets = 0, digits = 0, value = 0;
    for (size_t i = 0; i <= s.size(); ++i) {
        const char c = (i < s.size()) ? s[i] : '.';
        if (c >= '0' && c <= '9') {
            if (++digits > 3) return false;
            value = value * 10 + (c - '0');
            if (value > 255) return false;
        } else if (c == '.') {
            if (digits == 0) return false;
            ++octets; digits = 0; value = 0;
        } else {
            return false;
        }
    }
    return octets == 4;
}

} // namespace

const RouteIntent* RoutePlan::find(const std::string& ifname) const
{
    for (const RouteIntent& r : routes)
        if (r.ifname == ifname) return &r;
    return nullptr;
}

std::vector<std::string> split_dns(const std::string& field)
{
    std::vector<std::string> out;
    std::string cur;
    for (size_t i = 0; i <= field.size(); ++i) {
        const char c = (i < field.size()) ? field[i] : ' ';
        if (c == ' ' || c == '\t' || c == ',') {
            if (!cur.empty()) {
                // Duplicates dropped: two identical nameserver lines are not
                // redundancy, they are the same timeout twice.
                if (is_dotted_quad(cur) &&
                    std::find(out.begin(), out.end(), cur) == out.end())
                    out.push_back(cur);
                cur.clear();
            }
        } else {
            cur.push_back(c);
        }
    }
    return out;
}

int policy_rank(const UplinkStatus& u, const UplinkPolicy& policy)
{
    // An id beats a type name, exactly as in ConnectivityManager::select. Two
    // different rules for "which entry does this uplink match" would mean the
    // uplink the manager picks and the route the plan writes could disagree.
    for (size_t i = 0; i < policy.order.size(); ++i)
        if (policy.order[i] == u.id) return (int)i;
    for (size_t i = 0; i < policy.order.size(); ++i)
        if (policy.order[i] == uplink_type_name(u.type)) return (int)i;
    return -1;
}

RoutePlan plan_routes(const std::vector<UplinkStatus>& uplinks,
                      const UplinkPolicy& policy,
                      const std::string& active_id)
{
    RoutePlan plan;

    for (const UplinkStatus& u : uplinks) {
        if (u.info.ifname.empty()) continue;      // nothing to route through
        // Recorded even when it gets no route: this is the list of interfaces
        // reconciliation may touch, and an uplink that is DOWN is exactly the
        // one whose stale default route has to go.
        if (std::find(plan.managed_ifnames.begin(), plan.managed_ifnames.end(),
                      u.info.ifname) == plan.managed_ifnames.end())
            plan.managed_ifnames.push_back(u.info.ifname);

        // A route needs a working link and an address. "Connected" already
        // means addressed for every uplink in this codebase; the ipv4 check is
        // here so that an implementation which ever disagrees produces no
        // route rather than a route to nowhere.
        if (u.state != LinkState::Connected || u.info.ipv4.empty()) continue;

        // Eine gateway-lose Default-Route ist NUR auf einem Punkt-zu-Punkt-Link
        // sinnvoll (Mobilfunk-PPP: der Peer ist implizit). Auf einem
        // Broadcast-Link (Ethernet, WLAN) waere `default dev ethX scope link`
        // eine Route ins Leere -- und weil eth0 bei aktivem WLAN-Profil nur die
        // Fallback-Adresse OHNE Router traegt, bekam es genau so eine und
        // ueberschattete mit seiner niedrigen Metrik die funktionierende
        // Mobilfunk-Route. Gemessen 2026-09-26: kein Internet, bis diese Route
        // weg war. Also: kein Gateway + kein Mobilfunk -> keine Route.
        if (u.info.gateway.empty() && u.type != UplinkType::Cellular) continue;

        RouteIntent r;
        r.uplink_id = u.id;
        r.ifname    = u.info.ifname;
        r.gateway   = u.info.gateway;
        r.active    = (!active_id.empty() && u.id == active_id);

        const int rank = policy_rank(u, policy);
        r.metric = r.active ? kActiveRouteMetric
                            : (rank < 0 ? kUnrankedRouteMetric
                                        : (rank + 1) * kRouteMetricStep);
        plan.routes.push_back(r);
    }

    // Most preferred first. The order is what a reader of the status page and
    // a reader of `ip route` should both see, so it is sorted by the number
    // that actually decides rather than by registration order. Ties break on
    // the id so the output is stable -- an unstable order would make the
    // reconciliation log churn for no reason.
    std::sort(plan.routes.begin(), plan.routes.end(),
              [](const RouteIntent& a, const RouteIntent& b) {
                  if (a.metric != b.metric) return a.metric < b.metric;
                  return a.uplink_id < b.uplink_id;
              });

    // DNS: the active uplink and nobody else.
    //
    // This is the rule that stops an inactive cellular link from hijacking
    // name resolution. A modem that renews its lease every few minutes would
    // otherwise rewrite resolv.conf every few minutes with the carrier's
    // resolvers, no matter that every packet is leaving through Ethernet.
    for (const UplinkStatus& u : uplinks) {
        if (active_id.empty() || u.id != active_id) continue;
        // Nur ein Uplink, der seine Server SELBST kennt, darf die Datei
        // besitzen. Wer sie blos aus resolv.conf zurueckliest, wuerde
        // bestaetigen, was ohnehin dort steht -- auch wenn es die Server des
        // Mobilfunkanbieters sind und der Verkehr laengst wieder ueber
        // Ethernet geht. Genau so blieben die Resolver des Anbieters nach
        // einem Rueckfall fuer immer stehen.
        if (!u.info.dns_is_own) break;
        std::vector<std::string> servers = split_dns(u.info.dns);
        if (!servers.empty()) {
            plan.dns = std::move(servers);
            plan.dns_owner = u.id;
        }
        // No servers: owner stays empty, and the backend leaves the file
        // alone. See the note on RoutePlan::dns.
        break;
    }

    return plan;
}

}} // namespace machino::net
