#include "adapters/linux/linux_route_backend.hpp"

#include "core/log.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <net/if.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

static const char* MOD = "route";

namespace machino { namespace linuxsys {

namespace {

std::string read_file(const std::string& path)
{
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return std::string();
    std::string s;
    char buf[1024];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) s.append(buf, n);
    fclose(f);
    return s;
}

// One attribute appended to a netlink message, with the padding rtnetlink
// expects. Getting the padding wrong does not produce an error -- it produces
// a message the kernel parses as something else.
bool put_attr(struct nlmsghdr* nh, size_t cap, int type, const void* data, size_t len)
{
    const size_t need = NLMSG_ALIGN(nh->nlmsg_len) + RTA_LENGTH(len);
    if (need > cap) return false;
    struct rtattr* rta = (struct rtattr*)((char*)nh + NLMSG_ALIGN(nh->nlmsg_len));
    rta->rta_type = (unsigned short)type;
    rta->rta_len  = (unsigned short)RTA_LENGTH(len);
    memcpy(RTA_DATA(rta), data, len);
    nh->nlmsg_len = (unsigned)need;
    return true;
}

} // namespace

LinuxRouteBackend::LinuxRouteBackend(std::string proc_root, std::string resolv_path)
    : proc_(std::move(proc_root)), resolv_(std::move(resolv_path)) {}

bool LinuxRouteBackend::default_routes(std::vector<net::DefaultRoute>& out) const
{
    const std::string text = read_file(proc_ + "/net/route");
    // Empty is NOT "there are no default routes". /proc/net/route always has a
    // header line, so an empty read means the file could not be opened -- and
    // reporting that as "the table is empty" would have reconciliation delete
    // every route it knows about.
    if (text.empty()) return false;
    out = net::parse_proc_net_route(text);
    return true;
}

Result LinuxRouteBackend::route_op(int nlmsg_type, int flags, const std::string& ifname,
                                   const std::string& gateway, int metric)
{
    const unsigned oif = if_nametoindex(ifname.c_str());
    if (oif == 0) {
        LOGW(MOD, "%s: no such interface", ifname.c_str());
        return Result::error();
    }

    struct in_addr gw;
    memset(&gw, 0, sizeof gw);
    const bool have_gw = !gateway.empty();
    if (have_gw && inet_pton(AF_INET, gateway.c_str(), &gw) != 1) {
        LOGW(MOD, "%s: the gateway is not an IPv4 address", ifname.c_str());
        return Result::error();
    }

    char buf[512];
    memset(buf, 0, sizeof buf);
    struct nlmsghdr* nh = (struct nlmsghdr*)buf;
    nh->nlmsg_len   = NLMSG_LENGTH(sizeof(struct rtmsg));
    nh->nlmsg_type  = (unsigned short)nlmsg_type;
    nh->nlmsg_flags = (unsigned short)(NLM_F_REQUEST | NLM_F_ACK | flags);
    nh->nlmsg_seq   = 1;

    struct rtmsg* rtm = (struct rtmsg*)NLMSG_DATA(nh);
    rtm->rtm_family   = AF_INET;
    rtm->rtm_dst_len  = 0;                 // 0.0.0.0/0 -- the default route
    rtm->rtm_table    = RT_TABLE_MAIN;
    rtm->rtm_protocol = RTPROT_STATIC;
    // A route with a gateway reaches beyond the link; one without is the link
    // itself. Saying UNIVERSE for a gatewayless route makes the kernel refuse
    // it with ENETUNREACH, which reads like the network is down.
    rtm->rtm_scope    = have_gw ? RT_SCOPE_UNIVERSE : RT_SCOPE_LINK;
    rtm->rtm_type     = RTN_UNICAST;

    if (have_gw && !put_attr(nh, sizeof buf, RTA_GATEWAY, &gw.s_addr, sizeof gw.s_addr))
        return Result::error();
    const uint32_t oif32 = oif;
    if (!put_attr(nh, sizeof buf, RTA_OIF, &oif32, sizeof oif32)) return Result::error();
    const uint32_t prio = (uint32_t)(metric < 0 ? 0 : metric);
    if (!put_attr(nh, sizeof buf, RTA_PRIORITY, &prio, sizeof prio)) return Result::error();

    const int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
    if (fd < 0) { LOGW(MOD, "netlink socket: %s", strerror(errno)); return Result::error(); }

    struct sockaddr_nl kernel;
    memset(&kernel, 0, sizeof kernel);
    kernel.nl_family = AF_NETLINK;

    Result rc = Result::ok();
    if (sendto(fd, nh, nh->nlmsg_len, 0, (struct sockaddr*)&kernel, sizeof kernel) < 0) {
        LOGW(MOD, "netlink send: %s", strerror(errno));
        rc = Result::error();
    } else {
        // The ACK is read, not assumed. Without it a rejected route looks
        // exactly like an accepted one, and reconciliation would report
        // success and then find the same mismatch on every later pass.
        char ack[512];
        const ssize_t n = recv(fd, ack, sizeof ack, 0);
        if (n < 0) {
            LOGW(MOD, "netlink ack: %s", strerror(errno));
            rc = Result::error();
        } else {
            struct nlmsghdr* rh = (struct nlmsghdr*)ack;
            if (NLMSG_OK(rh, (unsigned)n) && rh->nlmsg_type == NLMSG_ERROR) {
                struct nlmsgerr* e = (struct nlmsgerr*)NLMSG_DATA(rh);
                const int err = -e->error;
                // EEXIST on add and ESRCH/ENOENT on delete both mean the table
                // already says what it was being asked to say. Treating those
                // as failures would turn a no-op into a permanent error on the
                // status page.
                if (err == 0) {
                    rc = Result::ok();
                } else if (nlmsg_type == RTM_NEWROUTE && err == EEXIST) {
                    rc = Result::ok();
                } else if (nlmsg_type == RTM_DELROUTE && (err == ESRCH || err == ENOENT)) {
                    rc = Result::ok();
                } else {
                    LOGW(MOD, "%s default dev %s metric %d: %s",
                         nlmsg_type == RTM_NEWROUTE ? "add" : "del",
                         ifname.c_str(), metric, strerror(err));
                    rc = Result::error();
                }
            }
        }
    }
    close(fd);
    return rc;
}

Result LinuxRouteBackend::add_default(const std::string& ifname, const std::string& gateway,
                                      int metric)
{
    return route_op(RTM_NEWROUTE, NLM_F_CREATE | NLM_F_EXCL, ifname, gateway, metric);
}

Result LinuxRouteBackend::del_default(const std::string& ifname, const std::string& gateway,
                                      int metric)
{
    return route_op(RTM_DELROUTE, 0, ifname, gateway, metric);
}

bool LinuxRouteBackend::dns(std::vector<std::string>& out) const
{
    const std::string text = read_file(resolv_);
    if (text.empty()) return false;
    out = net::parse_resolv_conf(text);
    return true;
}

Result LinuxRouteBackend::set_dns(const std::vector<std::string>& servers)
{
    // Refused, not obeyed. An empty list reaching here would empty the file and
    // take name resolution away from everything on the camera -- ntpd, ONVIF
    // lookups, any outbound push. The caller is supposed to not call; this is
    // the second line.
    if (servers.empty()) return Result::error();

    const std::string tmp = resolv_ + ".machino.tmp";
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) { LOGW(MOD, "%s: %s", tmp.c_str(), strerror(errno)); return Result::error(); }

    fprintf(f, "# written by machino - the active uplink owns this file\n");
    for (const std::string& s : servers) fprintf(f, "nameserver %s\n", s.c_str());

    // Flushed and synced BEFORE the rename. A rename is atomic with respect to
    // readers; it is not a promise that the bytes reached the medium, and a
    // power cut between the two leaves a resolv.conf of zero length.
    const bool wrote = fflush(f) == 0 && fsync(fileno(f)) == 0;
    fclose(f);
    if (!wrote) { unlink(tmp.c_str()); return Result::error(); }

    // Permissions set explicitly, not left to the umask.
    //
    // This process may have inherited umask 077 -- the WiFi role supervisor
    // runs with it so a wpa_supplicant config carrying a PSK is not world
    // readable. resolv.conf is not a secret, and silently downgrading a 0644
    // system file to root-only is the kind of change nobody finds later,
    // because on this camera everything runs as root anyway.
    if (chmod(tmp.c_str(), 0644) != 0)
        LOGW(MOD, "%s: chmod: %s", tmp.c_str(), strerror(errno));

    if (rename(tmp.c_str(), resolv_.c_str()) != 0) {
        LOGW(MOD, "%s: rename: %s", resolv_.c_str(), strerror(errno));
        unlink(tmp.c_str());
        return Result::error();
    }
    return Result::ok();
}

}} // namespace machino::linuxsys
