// AP-M5: who gets the default route, and whose DNS the system uses.
//
// Every case here is one that produced a real misbehaviour before the decision
// was centralised, or would produce one if the reconciliation got careless:
// two uplinks both holding a default route, a preferred uplink with a cable
// and no internet keeping the traffic, an inactive modem renewing its lease
// and quietly taking over name resolution, and a route through an interface
// that is none of machino's business.
#include "core/net/route_manager.hpp"
#include "core/net/route_plan.hpp"
#include <cstdio>
#include <string>
#include <vector>

using namespace machino;
using namespace machino::net;

extern int g_fail_ext, g_pass_ext;
#define TCHECK(cond) do { if (cond) { ++g_pass_ext; } else { ++g_fail_ext; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

namespace {

UplinkStatus up(const char* id, UplinkType t, const char* ifname, const char* ip,
                const char* gw, const char* dns = "", bool active = false,
                LinkState st = LinkState::Connected)
{
    UplinkStatus u;
    u.id = id; u.type = t; u.state = st; u.active = active;
    u.internet = (st == LinkState::Connected);
    u.info.ifname = ifname; u.info.ipv4 = ip; u.info.gateway = gw; u.info.dns = dns;
    // The fixtures describe uplinks that KNOW their servers. The case where an
    // uplink only reads resolv.conf back has its own test, because it is the
    // interesting one.
    u.info.dns_is_own = (dns[0] != '\0');
    return u;
}

// A routing table the test owns, and a record of every operation asked of it.
class FakeRouteBackend : public IRouteBackend {
public:
    std::vector<DefaultRoute> table;
    std::vector<std::string>  resolv;
    std::vector<std::string>  ops;         // "add eth0 1.2.3.1 10", "del ..."
    bool read_ok = true;
    bool dns_read_ok = true;
    bool add_fails = false;

    bool default_routes(std::vector<DefaultRoute>& out) const override
    {
        if (!read_ok) return false;
        out = table;
        return true;
    }
    Result add_default(const std::string& ifname, const std::string& gw, int metric) override
    {
        ops.push_back("add " + ifname + " " + (gw.empty() ? "-" : gw) + " " + std::to_string(metric));
        if (add_fails) return Result::error();
        DefaultRoute r; r.ifname = ifname; r.gateway = gw; r.metric = metric;
        table.push_back(r);
        return Result::ok();
    }
    Result del_default(const std::string& ifname, const std::string& gw, int metric) override
    {
        ops.push_back("del " + ifname + " " + (gw.empty() ? "-" : gw) + " " + std::to_string(metric));
        for (size_t i = 0; i < table.size(); ++i) {
            if (table[i].ifname == ifname && table[i].gateway == gw && table[i].metric == metric) {
                table.erase(table.begin() + (long)i);
                return Result::ok();
            }
        }
        return Result::ok();
    }
    Result set_dns(const std::vector<std::string>& s) override
    {
        if (s.empty()) return Result::error();
        ops.push_back("dns " + s[0]);
        resolv = s;
        return Result::ok();
    }
    bool dns(std::vector<std::string>& out) const override
    {
        if (!dns_read_ok) return false;
        out = resolv;
        return true;
    }

    bool has(const std::string& ifname, const std::string& gw, int metric) const
    {
        for (const DefaultRoute& r : table)
            if (r.ifname == ifname && r.gateway == gw && r.metric == metric) return true;
        return false;
    }
    int count_for(const std::string& ifname) const
    {
        int n = 0;
        for (const DefaultRoute& r : table) if (r.ifname == ifname) ++n;
        return n;
    }
    bool did(const std::string& op) const
    {
        for (const std::string& s : ops) if (s == op) return true;
        return false;
    }
    int index_of(const std::string& op) const
    {
        for (size_t i = 0; i < ops.size(); ++i) if (ops[i] == op) return (int)i;
        return -1;
    }
};

// ------------------------------------------------------------- the plan ----

void test_the_active_uplink_gets_the_lowest_metric()
{
    // The case the whole file exists for. Ethernet is first in the policy, has
    // a cable and a default route -- and no internet. The manager therefore
    // makes cellular active, and before this the kernel went on sending every
    // packet down the Ethernet route because nobody told it.
    std::vector<UplinkStatus> u = {
        up("eth0", UplinkType::Ethernet, "eth0", "192.168.1.10", "192.168.1.1"),
        up("cellular", UplinkType::Cellular, "usb0", "10.5.6.7", "10.5.6.1", "", true),
    };
    const RoutePlan p = plan_routes(u, UplinkPolicy{}, "cellular");

    TCHECK(p.routes.size() == 2);
    TCHECK(p.routes[0].uplink_id == "cellular");          // sorted by metric
    TCHECK(p.routes[0].metric == kActiveRouteMetric);
    TCHECK(p.routes[0].active);
    TCHECK(p.routes[1].uplink_id == "eth0");
    TCHECK(p.routes[1].metric == 100);                    // rank 0 -> 100
    TCHECK(p.routes[0].metric < p.routes[1].metric);
}

void test_metrics_follow_the_policy_order_not_the_registration_order()
{
    // The old numbers lived in three shell scripts: eth 0, wlan 200, cellular
    // 300. Changing the policy to "cellular first" changed the status page and
    // nothing else. It has to change the metrics too, or the preference is
    // decorative.
    std::vector<UplinkStatus> u = {
        up("eth0", UplinkType::Ethernet, "eth0", "192.168.1.10", "192.168.1.1"),
        up("wlan0", UplinkType::Wifi, "wlan0", "192.168.2.10", "192.168.2.1"),
        up("cellular", UplinkType::Cellular, "usb0", "10.5.6.7", "10.5.6.1"),
    };
    UplinkPolicy p;
    p.order = {"cellular", "wifi", "ethernet"};
    const RoutePlan plan = plan_routes(u, p, "");          // nothing active yet

    TCHECK(plan.routes.size() == 3);
    TCHECK(plan.routes[0].uplink_id == "cellular" && plan.routes[0].metric == 100);
    TCHECK(plan.routes[1].uplink_id == "wlan0"    && plan.routes[1].metric == 200);
    TCHECK(plan.routes[2].uplink_id == "eth0"     && plan.routes[2].metric == 300);
}

void test_an_uplink_outside_the_policy_is_reachable_but_never_preferred()
{
    std::vector<UplinkStatus> u = {
        up("eth0", UplinkType::Ethernet, "eth0", "192.168.1.10", "192.168.1.1"),
        up("lte1", UplinkType::Cellular, "wwan1", "10.9.9.9", "10.9.9.1"),
    };
    UplinkPolicy p;
    p.order = {"ethernet"};                                 // cellular unmentioned
    const RoutePlan plan = plan_routes(u, p, "");
    TCHECK(plan.routes[0].uplink_id == "eth0" && plan.routes[0].metric == 100);
    TCHECK(plan.routes[1].uplink_id == "lte1" && plan.routes[1].metric == kUnrankedRouteMetric);
}

void test_an_id_entry_outranks_a_type_entry()
{
    // Same rule as the manager's selection. Two rules would mean the uplink
    // that is active and the uplink with the best metric could be different
    // ones, which is the bug this file is about.
    std::vector<UplinkStatus> u = {
        up("lte0", UplinkType::Cellular, "usb0", "10.1.1.2", "10.1.1.1"),
        up("lte1", UplinkType::Cellular, "usb1", "10.2.2.2", "10.2.2.1"),
    };
    UplinkPolicy p;
    p.order = {"lte1", "cellular"};
    TCHECK(policy_rank(u[1], p) == 0);      // matched by id
    TCHECK(policy_rank(u[0], p) == 1);      // matched by type
}

void test_an_unconnected_uplink_gets_no_route_but_stays_managed()
{
    // The distinction that makes reconciliation safe: the interface is still
    // ours -- so a stale route on it may be removed -- but it gets no route of
    // its own.
    std::vector<UplinkStatus> u = {
        up("eth0", UplinkType::Ethernet, "eth0", "192.168.1.10", "192.168.1.1"),
        up("cellular", UplinkType::Cellular, "usb0", "", "", "", false, LinkState::Down),
    };
    const RoutePlan p = plan_routes(u, UplinkPolicy{}, "eth0");
    TCHECK(p.routes.size() == 1 && p.routes[0].uplink_id == "eth0");
    TCHECK(p.managed_ifnames.size() == 2);
    TCHECK(p.find("usb0") == nullptr);
}

void test_an_uplink_with_no_interface_name_is_not_managed_at_all()
{
    // A cellular uplink before the modem has enumerated has no interface. It
    // must not contribute an empty ifname to the managed set -- an empty name
    // would match an empty name, and reconciliation would start deleting
    // routes whose interface it failed to parse.
    std::vector<UplinkStatus> u = {
        up("cellular", UplinkType::Cellular, "", "", "", "", false, LinkState::Absent),
    };
    const RoutePlan p = plan_routes(u, UplinkPolicy{}, "");
    TCHECK(p.managed_ifnames.empty());
    TCHECK(p.routes.empty());
}

void test_a_gatewayless_uplink_still_gets_a_route()
{
    // A point-to-point modem link can have no gateway at all. Dropping it
    // would leave the one uplink that works without a default route.
    std::vector<UplinkStatus> u = {
        up("cellular", UplinkType::Cellular, "usb0", "10.5.6.7", "", "", true),
    };
    const RoutePlan p = plan_routes(u, UplinkPolicy{}, "cellular");
    TCHECK(p.routes.size() == 1);
    TCHECK(p.routes[0].gateway.empty());
    TCHECK(p.routes[0].metric == kActiveRouteMetric);
}

// ------------------------------------------------------------------ DNS ----

void test_only_the_active_uplink_owns_dns()
{
    std::vector<UplinkStatus> u = {
        up("eth0", UplinkType::Ethernet, "eth0", "192.168.1.10", "192.168.1.1", "192.168.1.1", true),
        up("cellular", UplinkType::Cellular, "usb0", "10.5.6.7", "10.5.6.1", "10.74.210.210 10.74.210.211"),
    };
    const RoutePlan p = plan_routes(u, UplinkPolicy{}, "eth0");
    TCHECK(p.dns_owner == "eth0");
    TCHECK(p.dns.size() == 1 && p.dns[0] == "192.168.1.1");
}

void test_an_inactive_cellular_lease_cannot_hijack_dns()
{
    // The modem renews every few minutes. Before this, the DHCP hook rewrote
    // resolv.conf on every renewal with the carrier's resolvers, while every
    // packet was still leaving through Ethernet -- so name resolution went out
    // over a link the traffic never used, and nobody could see why lookups
    // were slow.
    FakeRouteBackend be;
    be.resolv = {"192.168.1.1"};
    RouteManager rm(be);

    std::vector<UplinkStatus> u = {
        up("eth0", UplinkType::Ethernet, "eth0", "192.168.1.10", "192.168.1.1", "192.168.1.1", true),
        up("cellular", UplinkType::Cellular, "usb0", "10.5.6.7", "10.5.6.1", "10.74.210.210"),
    };
    const ReconcileReport r = rm.reconcile(plan_routes(u, UplinkPolicy{}, "eth0"));
    TCHECK(!r.dns_written);
    TCHECK(be.resolv.size() == 1 && be.resolv[0] == "192.168.1.1");
}

void test_an_active_uplink_with_no_dns_leaves_the_file_alone()
{
    // Writing an empty resolv.conf because one lease was quiet about DNS takes
    // name resolution away from the whole camera. Leaving the old servers is
    // wrong in a small way; wiping them is wrong in a way nobody recovers from
    // without a serial console.
    FakeRouteBackend be;
    be.resolv = {"192.168.1.1"};
    RouteManager rm(be);

    std::vector<UplinkStatus> u = {
        up("cellular", UplinkType::Cellular, "usb0", "10.5.6.7", "10.5.6.1", "", true),
    };
    const RoutePlan p = plan_routes(u, UplinkPolicy{}, "cellular");
    TCHECK(p.dns_owner.empty() && p.dns.empty());

    rm.reconcile(p);
    TCHECK(be.resolv.size() == 1 && be.resolv[0] == "192.168.1.1");
}

void test_dns_is_written_when_the_active_uplink_changes()
{
    FakeRouteBackend be;
    be.resolv = {"192.168.1.1"};
    RouteManager rm(be);

    std::vector<UplinkStatus> u = {
        up("eth0", UplinkType::Ethernet, "eth0", "192.168.1.10", "192.168.1.1", "192.168.1.1"),
        up("cellular", UplinkType::Cellular, "usb0", "10.5.6.7", "10.5.6.1", "10.74.210.210 10.74.210.211", true),
    };
    const ReconcileReport r = rm.reconcile(plan_routes(u, UplinkPolicy{}, "cellular"));
    TCHECK(r.dns_written);
    TCHECK(be.resolv.size() == 2 && be.resolv[0] == "10.74.210.210" && be.resolv[1] == "10.74.210.211");
}

void test_an_uplink_that_only_reads_resolv_conf_back_does_not_own_dns()
{
    // LinuxNetif answers dns() by parsing /etc/resolv.conf -- true as a
    // statement about the traffic, useless as an answer to "what did this
    // uplink hand us". Once machino writes that file, Ethernet would report
    // machino's own servers as its own.
    UplinkStatus eth = up("eth0", UplinkType::Ethernet, "eth0", "192.168.1.10",
                          "192.168.1.1", "10.74.210.210", true);
    eth.info.dns_is_own = false;                 // read back, not learned
    const RoutePlan p = plan_routes({eth}, UplinkPolicy{}, "eth0");
    TCHECK(p.dns_owner.empty());
    TCHECK(p.dns.empty());
    // The route is unaffected -- only DNS ownership is in question here.
    TCHECK(p.routes.size() == 1);
}

void test_failing_back_to_ethernet_puts_the_old_resolvers_back()
{
    // The whole reason ownership is tracked. Cellular becomes active, machino
    // writes the carrier's resolvers; Ethernet comes back, and it cannot name
    // its own servers because on Linux it learns them from the very file
    // machino just overwrote. Without a snapshot the camera would resolve
    // names through a modem it is no longer using -- for good, because the
    // wrong answer keeps matching itself.
    FakeRouteBackend be;
    be.resolv = {"192.168.1.1"};
    RouteManager rm(be);

    UplinkStatus eth = up("eth0", UplinkType::Ethernet, "eth0", "192.168.1.10",
                          "192.168.1.1", "192.168.1.1");
    eth.info.dns_is_own = false;
    UplinkStatus cell = up("cellular", UplinkType::Cellular, "usb0", "10.5.6.7",
                           "10.5.6.1", "10.74.210.210 10.74.210.211", true);

    const ReconcileReport a = rm.reconcile(plan_routes({eth, cell}, UplinkPolicy{}, "cellular"));
    TCHECK(a.dns_written);
    TCHECK(be.resolv.size() == 2 && be.resolv[0] == "10.74.210.210");

    cell.active = false;
    eth.active = true;
    const ReconcileReport b = rm.reconcile(plan_routes({eth, cell}, UplinkPolicy{}, "eth0"));
    TCHECK(b.dns_restored);
    TCHECK(be.resolv.size() == 1 && be.resolv[0] == "192.168.1.1");
}

void test_the_snapshot_is_taken_once_and_not_overwritten_with_our_own()
{
    // Re-snapshotting on every write would capture machino's own servers, and
    // the restore would put back exactly what it was meant to undo -- the same
    // as having no restore at all.
    FakeRouteBackend be;
    be.resolv = {"192.168.1.1"};
    RouteManager rm(be);

    UplinkStatus cell = up("cellular", UplinkType::Cellular, "usb0", "10.5.6.7",
                           "10.5.6.1", "10.74.210.210", true);
    rm.reconcile(plan_routes({cell}, UplinkPolicy{}, "cellular"));

    // The modem renews and hands out a different resolver.
    cell.info.dns = "10.74.210.211";
    rm.reconcile(plan_routes({cell}, UplinkPolicy{}, "cellular"));
    TCHECK(be.resolv.size() == 1 && be.resolv[0] == "10.74.210.211");

    // Cellular goes away entirely.
    const ReconcileReport r = rm.reconcile(plan_routes({}, UplinkPolicy{}, ""));
    TCHECK(r.dns_restored);
    TCHECK(be.resolv.size() == 1 && be.resolv[0] == "192.168.1.1");
}

void test_giving_up_ownership_never_empties_the_file()
{
    // No snapshot, because resolv.conf could not be read when ownership was
    // taken. Leaving machino's servers is wrong; writing an empty file takes
    // name resolution away from the whole camera, which is worse. It says so
    // rather than doing either quietly.
    FakeRouteBackend be;
    be.dns_read_ok = false;
    RouteManager rm(be);

    UplinkStatus cell = up("cellular", UplinkType::Cellular, "usb0", "10.5.6.7",
                           "10.5.6.1", "10.74.210.210", true);
    TCHECK(rm.reconcile(plan_routes({cell}, UplinkPolicy{}, "cellular")).dns_written);

    be.dns_read_ok = true;
    const ReconcileReport r = rm.reconcile(plan_routes({}, UplinkPolicy{}, ""));
    TCHECK(!r.dns_restored);
    TCHECK(!r.error.empty());
    TCHECK(!be.resolv.empty());        // not emptied
}

void test_ownership_is_not_given_back_twice()
{
    FakeRouteBackend be;
    be.resolv = {"192.168.1.1"};
    RouteManager rm(be);
    UplinkStatus cell = up("cellular", UplinkType::Cellular, "usb0", "10.5.6.7",
                           "10.5.6.1", "10.74.210.210", true);
    rm.reconcile(plan_routes({cell}, UplinkPolicy{}, "cellular"));
    TCHECK(rm.reconcile(plan_routes({}, UplinkPolicy{}, "")).dns_restored);

    const size_t ops = be.ops.size();
    for (int i = 0; i < 3; ++i) {
        const ReconcileReport r = rm.reconcile(plan_routes({}, UplinkPolicy{}, ""));
        TCHECK(!r.dns_restored);
        TCHECK(r.error.empty());
    }
    TCHECK(be.ops.size() == ops);
}

void test_dns_is_not_rewritten_when_it_already_matches()
{
    // reconcile() runs on a timer. A version that wrote unconditionally would
    // rewrite resolv.conf every second for the life of the camera.
    FakeRouteBackend be;
    be.resolv = {"192.168.1.1"};
    RouteManager rm(be);
    std::vector<UplinkStatus> u = {
        up("eth0", UplinkType::Ethernet, "eth0", "192.168.1.10", "192.168.1.1", "192.168.1.1", true),
    };
    const RoutePlan p = plan_routes(u, UplinkPolicy{}, "eth0");
    rm.reconcile(p);
    const size_t after_first = be.ops.size();
    rm.reconcile(p);
    rm.reconcile(p);
    TCHECK(be.ops.size() == after_first);
}

void test_split_dns_drops_rubbish_and_duplicates()
{
    const std::vector<std::string> a = split_dns("1.1.1.1 8.8.8.8");
    TCHECK(a.size() == 2 && a[0] == "1.1.1.1" && a[1] == "8.8.8.8");
    // A malformed entry is worse than a missing one: the resolver waits for it.
    TCHECK(split_dns("not-an-address").empty());
    TCHECK(split_dns("1.1.1.1 1.1.1.1").size() == 1);
    TCHECK(split_dns("999.1.1.1").empty());
    TCHECK(split_dns("").empty());
    // A dotted name is not an address, and neither is a prefix.
    TCHECK(split_dns("1.1.1").empty());
    TCHECK(split_dns("1.1.1.1.1").empty());
}

// ------------------------------------------------------- reconciliation ----

void test_two_default_routes_resolve_to_one_deterministic_winner()
{
    // The state the camera is actually in after both DHCP hooks have run:
    // eth0 at metric 0 from the boot script, usb0 at 300 from the cellular
    // hook. Which one wins is then a matter of insertion order -- which is to
    // say, of the boot sequence.
    FakeRouteBackend be;
    be.table = {
        {"eth0", "192.168.1.1", 0},
        {"usb0", "10.5.6.1", 300},
    };
    RouteManager rm(be);

    std::vector<UplinkStatus> u = {
        up("eth0", UplinkType::Ethernet, "eth0", "192.168.1.10", "192.168.1.1"),
        up("cellular", UplinkType::Cellular, "usb0", "10.5.6.7", "10.5.6.1", "", true),
    };
    const ReconcileReport r = rm.reconcile(plan_routes(u, UplinkPolicy{}, "cellular"));

    TCHECK(!r.read_failed && r.error.empty());
    TCHECK(be.has("usb0", "10.5.6.1", kActiveRouteMetric));
    TCHECK(be.has("eth0", "192.168.1.1", 100));
    TCHECK(!be.has("eth0", "192.168.1.1", 0));     // the ambiguous metric-0 route is gone
    TCHECK(!be.has("usb0", "10.5.6.1", 300));
    TCHECK(be.count_for("eth0") == 1 && be.count_for("usb0") == 1);
}

void test_the_new_route_goes_in_before_the_old_one_comes_out()
{
    // A camera is reached over the network it is reconfiguring. Delete-then-add
    // leaves a window with no route at all, and if the add fails the window
    // never closes.
    FakeRouteBackend be;
    be.table = {{"eth0", "192.168.1.1", 0}};
    RouteManager rm(be);
    std::vector<UplinkStatus> u = {
        up("eth0", UplinkType::Ethernet, "eth0", "192.168.1.10", "192.168.1.1", "", true),
    };
    rm.reconcile(plan_routes(u, UplinkPolicy{}, "eth0"));

    const int add = be.index_of("add eth0 192.168.1.1 " + std::to_string(kActiveRouteMetric));
    const int del = be.index_of("del eth0 192.168.1.1 0");
    TCHECK(add >= 0 && del >= 0);
    TCHECK(add < del);
}

void test_a_foreign_default_route_is_never_touched()
{
    // A VPN, a second NIC, whatever the operator added. Deleting it because it
    // was in the way is how a remote camera stops answering, and whoever it
    // happens to has no way to find out why.
    FakeRouteBackend be;
    be.table = {
        {"tun0", "10.8.0.1", 50},
        {"eth0", "192.168.1.1", 0},
    };
    RouteManager rm(be);
    std::vector<UplinkStatus> u = {
        up("eth0", UplinkType::Ethernet, "eth0", "192.168.1.10", "192.168.1.1", "", true),
    };
    rm.reconcile(plan_routes(u, UplinkPolicy{}, "eth0"));

    TCHECK(be.has("tun0", "10.8.0.1", 50));
    TCHECK(!be.did("del tun0 10.8.0.1 50"));
}

void test_a_stale_route_on_our_own_interface_is_removed()
{
    // The modem lost its lease. The hook's route stays behind, and with a
    // metric of 300 it is still the only default route once Ethernet is gone.
    FakeRouteBackend be;
    be.table = {{"usb0", "10.5.6.1", 300}};
    RouteManager rm(be);
    std::vector<UplinkStatus> u = {
        up("cellular", UplinkType::Cellular, "usb0", "", "", "", false, LinkState::Down),
    };
    const ReconcileReport r = rm.reconcile(plan_routes(u, UplinkPolicy{}, ""));
    TCHECK(r.removed == 1);
    TCHECK(be.table.empty());
}

void test_an_unreadable_table_changes_nothing()
{
    // Without knowing what is there, "what should not be there" is
    // "everything". Doing nothing is the only safe answer.
    FakeRouteBackend be;
    be.read_ok = false;
    be.table = {{"eth0", "192.168.1.1", 0}};
    RouteManager rm(be);
    std::vector<UplinkStatus> u = {
        up("eth0", UplinkType::Ethernet, "eth0", "192.168.1.10", "192.168.1.1", "", true),
    };
    const ReconcileReport r = rm.reconcile(plan_routes(u, UplinkPolicy{}, "eth0"));
    TCHECK(r.read_failed);
    TCHECK(!r.changed());
    TCHECK(be.ops.empty());
}

void test_reconcile_is_a_no_op_once_the_table_matches()
{
    FakeRouteBackend be;
    RouteManager rm(be);
    std::vector<UplinkStatus> u = {
        up("eth0", UplinkType::Ethernet, "eth0", "192.168.1.10", "192.168.1.1", "192.168.1.1", true),
        up("cellular", UplinkType::Cellular, "usb0", "10.5.6.7", "10.5.6.1"),
    };
    const RoutePlan p = plan_routes(u, UplinkPolicy{}, "eth0");

    const ReconcileReport first = rm.reconcile(p);
    TCHECK(first.changed());
    const size_t ops_after_first = be.ops.size();

    for (int i = 0; i < 5; ++i) {
        const ReconcileReport again = rm.reconcile(p);
        TCHECK(!again.changed());
    }
    TCHECK(be.ops.size() == ops_after_first);
}

void test_a_failed_add_does_not_remove_the_route_it_was_replacing()
{
    // The dangerous ordering failure: the new route could not be installed, so
    // the old one is the only way out. Removing it anyway would leave the
    // camera with no default route and no way to get one.
    FakeRouteBackend be;
    be.table = {{"eth0", "192.168.1.1", 0}};
    be.add_fails = true;
    RouteManager rm(be);
    std::vector<UplinkStatus> u = {
        up("eth0", UplinkType::Ethernet, "eth0", "192.168.1.10", "192.168.1.1", "", true),
    };
    const ReconcileReport r = rm.reconcile(plan_routes(u, UplinkPolicy{}, "eth0"));

    TCHECK(r.added == 0);
    TCHECK(!r.error.empty());
    TCHECK(be.has("eth0", "192.168.1.1", 0));      // still reachable
}

void test_an_unreadable_resolv_conf_is_written_rather_than_assumed_correct()
{
    FakeRouteBackend be;
    be.dns_read_ok = false;
    RouteManager rm(be);
    std::vector<UplinkStatus> u = {
        up("eth0", UplinkType::Ethernet, "eth0", "192.168.1.10", "192.168.1.1", "192.168.1.1", true),
    };
    const ReconcileReport r = rm.reconcile(plan_routes(u, UplinkPolicy{}, "eth0"));
    TCHECK(r.dns_written);
}

} // namespace

void run_route_plan_tests()
{
    test_the_active_uplink_gets_the_lowest_metric();
    test_metrics_follow_the_policy_order_not_the_registration_order();
    test_an_uplink_outside_the_policy_is_reachable_but_never_preferred();
    test_an_id_entry_outranks_a_type_entry();
    test_an_unconnected_uplink_gets_no_route_but_stays_managed();
    test_an_uplink_with_no_interface_name_is_not_managed_at_all();
    test_a_gatewayless_uplink_still_gets_a_route();

    test_only_the_active_uplink_owns_dns();
    test_an_inactive_cellular_lease_cannot_hijack_dns();
    test_an_active_uplink_with_no_dns_leaves_the_file_alone();
    test_dns_is_written_when_the_active_uplink_changes();
    test_an_uplink_that_only_reads_resolv_conf_back_does_not_own_dns();
    test_failing_back_to_ethernet_puts_the_old_resolvers_back();
    test_the_snapshot_is_taken_once_and_not_overwritten_with_our_own();
    test_giving_up_ownership_never_empties_the_file();
    test_ownership_is_not_given_back_twice();
    test_dns_is_not_rewritten_when_it_already_matches();
    test_split_dns_drops_rubbish_and_duplicates();

    test_two_default_routes_resolve_to_one_deterministic_winner();
    test_the_new_route_goes_in_before_the_old_one_comes_out();
    test_a_foreign_default_route_is_never_touched();
    test_a_stale_route_on_our_own_interface_is_removed();
    test_an_unreadable_table_changes_nothing();
    test_reconcile_is_a_no_op_once_the_table_matches();
    test_a_failed_add_does_not_remove_the_route_it_was_replacing();
    test_an_unreadable_resolv_conf_is_written_rather_than_assumed_correct();
}
