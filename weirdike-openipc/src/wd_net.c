/*
 * weirdiked -- TUN device and the two IKE sockets.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "wd_net.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <linux/if_tun.h>
#include <net/route.h>

static void seterr(char *err, size_t cap, const char *what)
{
    if (err && cap) snprintf(err, cap, "%s: %s", what, strerror(errno));
}

int wd_tun_open(const char *ifname, char *err, size_t errcap)
{
    int fd = open("/dev/net/tun", O_RDWR);
    if (fd < 0) {
        if (errno == ENOENT && err && errcap) {
            snprintf(err, errcap,
                     "/dev/net/tun is missing -- the tun module is not loaded "
                     "(the image ships tun.ko; add a line 'tun' to /etc/modules)");
        } else {
            seterr(err, errcap, "open /dev/net/tun");
        }
        return -1;
    }

    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    ifr.ifr_flags = IFF_TUN | IFF_NO_PI;
    snprintf(ifr.ifr_name, IFNAMSIZ, "%s", ifname);

    if (ioctl(fd, TUNSETIFF, &ifr) < 0) {
        seterr(err, errcap, "TUNSETIFF");
        close(fd);
        return -1;
    }

    int fl = fcntl(fd, F_GETFL, 0);
    if (fl >= 0) fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    return fd;
}

int wd_tun_configure(const char *ifname, const uint8_t ip[4], uint8_t prefix, int mtu,
                     char *err, size_t errcap)
{
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) { seterr(err, errcap, "socket"); return -1; }

    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    snprintf(ifr.ifr_name, IFNAMSIZ, "%s", ifname);

    struct sockaddr_in *sin = (struct sockaddr_in *)&ifr.ifr_addr;
    sin->sin_family = AF_INET;
    memcpy(&sin->sin_addr, ip, 4);
    if (ioctl(s, SIOCSIFADDR, &ifr) < 0) { seterr(err, errcap, "SIOCSIFADDR"); close(s); return -1; }

    uint32_t mask = (prefix == 0) ? 0 : htonl(0xffffffffu << (32 - prefix));
    memset(&ifr.ifr_netmask, 0, sizeof(ifr.ifr_netmask));
    sin = (struct sockaddr_in *)&ifr.ifr_netmask;
    sin->sin_family = AF_INET;
    memcpy(&sin->sin_addr, &mask, 4);
    if (ioctl(s, SIOCSIFNETMASK, &ifr) < 0) { seterr(err, errcap, "SIOCSIFNETMASK"); close(s); return -1; }

    ifr.ifr_mtu = mtu;
    if (ioctl(s, SIOCSIFMTU, &ifr) < 0) { seterr(err, errcap, "SIOCSIFMTU"); close(s); return -1; }

    if (ioctl(s, SIOCGIFFLAGS, &ifr) < 0) { seterr(err, errcap, "SIOCGIFFLAGS"); close(s); return -1; }
    ifr.ifr_flags |= IFF_UP | IFF_RUNNING;
    if (ioctl(s, SIOCSIFFLAGS, &ifr) < 0) { seterr(err, errcap, "SIOCSIFFLAGS"); close(s); return -1; }

    close(s);
    return 0;
}

int wd_route_dev(const char *ifname, const uint8_t net[4], uint8_t prefix, int add,
                 char *err, size_t errcap)
{
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) { seterr(err, errcap, "socket"); return -1; }

    struct rtentry rt;
    memset(&rt, 0, sizeof(rt));

    uint32_t mask = (prefix == 0) ? 0 : htonl(0xffffffffu << (32 - prefix));

    struct sockaddr_in *dst = (struct sockaddr_in *)&rt.rt_dst;
    dst->sin_family = AF_INET;
    memcpy(&dst->sin_addr, net, 4);
    dst->sin_addr.s_addr &= mask;           /* the kernel insists on a clean network address */

    struct sockaddr_in *gen = (struct sockaddr_in *)&rt.rt_genmask;
    gen->sin_family = AF_INET;
    memcpy(&gen->sin_addr, &mask, 4);

    ((struct sockaddr_in *)&rt.rt_gateway)->sin_family = AF_INET;

    char dev[16];
    snprintf(dev, sizeof(dev), "%s", ifname);
    rt.rt_dev   = dev;
    rt.rt_flags = RTF_UP;                   /* device route: no RTF_GATEWAY */

    int rc = ioctl(s, add ? SIOCADDRT : SIOCDELRT, &rt);
    if (rc < 0 && !add && errno == ESRCH) rc = 0;   /* already gone: cleanup is idempotent */
    if (rc < 0) seterr(err, errcap, add ? "SIOCADDRT" : "SIOCDELRT");
    close(s);
    return rc < 0 ? -1 : 0;
}

int wd_tun_down(const char *ifname, char *err, size_t errcap)
{
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) { seterr(err, errcap, "socket"); return -1; }

    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    snprintf(ifr.ifr_name, IFNAMSIZ, "%s", ifname);
    if (ioctl(s, SIOCGIFFLAGS, &ifr) < 0) { seterr(err, errcap, "SIOCGIFFLAGS"); close(s); return -1; }
    ifr.ifr_flags &= ~(IFF_UP | IFF_RUNNING);
    if (ioctl(s, SIOCSIFFLAGS, &ifr) < 0) { seterr(err, errcap, "SIOCSIFFLAGS"); close(s); return -1; }
    close(s);
    return 0;
}

int wd_udp_open(uint16_t port, char *err, size_t errcap)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { seterr(err, errcap, "socket"); return -1; }

    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons(port);
    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) < 0) {
        seterr(err, errcap, "bind");
        close(fd);
        return -1;
    }

    int fl = fcntl(fd, F_GETFL, 0);
    if (fl >= 0) fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    return fd;
}

int wd_source_addr_for(const uint8_t dst[4], uint8_t out[4])
{
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) return -1;

    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(500);
    memcpy(&a.sin_addr, dst, 4);

    /* connect() on UDP sends nothing; it only picks a route and a source. */
    if (connect(s, (struct sockaddr *)&a, sizeof(a)) < 0) { close(s); return -1; }

    struct sockaddr_in me;
    socklen_t ml = sizeof(me);
    if (getsockname(s, (struct sockaddr *)&me, &ml) < 0) { close(s); return -1; }
    close(s);

    memcpy(out, &me.sin_addr, 4);
    if (out[0] == 0 && out[1] == 0 && out[2] == 0 && out[3] == 0) return -1;
    return 0;
}

int wd_resolve4(const char *host, uint8_t out[4])
{
    struct in_addr lit;
    if (inet_pton(AF_INET, host, &lit) == 1) { memcpy(out, &lit, 4); return 0; }

    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo(host, NULL, &hints, &res) != 0 || !res) return -1;

    struct sockaddr_in *sin = (struct sockaddr_in *)res->ai_addr;
    memcpy(out, &sin->sin_addr, 4);
    freeaddrinfo(res);
    return 0;
}

uint32_t wd_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000));
}
