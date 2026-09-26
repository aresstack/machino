/*
 * weirdiked -- the tunnel route manager (AP6).
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "wd_routes.h"
#include "wd_net.h"

#include <stdio.h>
#include <string.h>

static const char *g_procfile = NULL;

void wd_routes_set_procfile(const char *path) { g_procfile = path; }

static uint32_t u32(const uint8_t b[4])
{
    return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | b[3];
}

static uint32_t mask_of(uint8_t prefix)
{
    return prefix == 0 ? 0u : 0xffffffffu << (32 - prefix);
}

static int same_route(const wd_route *a, const wd_route *b)
{
    return a->prefix == b->prefix && memcmp(a->net, b->net, 4) == 0;
}

/* Directly-connected nets from /proc/net/route: RTF_UP(0x1) set, RTF_GATEWAY
 * (0x2) clear. Fields are hex, little-endian, tab-separated:
 *   Iface Destination Gateway Flags RefCnt Use Metric Mask ...
 * A desired tunnel route collides when its network EQUALS a connected net
 * (same address after masking with the SHORTER prefix) -- that is the
 * "VPN wants the camera's own LAN" case AP6 forbids. The tunnel's OWN
 * ipsec0 connected route (added by wd_tun_configure) is skipped by ifname. */
static int collides_with_local(const wd_route *r, const char *ifname,
                               char *err, size_t errcap)
{
    FILE *f = fopen(g_procfile ? g_procfile : "/proc/net/route", "r");
    if (!f) return 0;                 /* cannot read -> do not block (best effort) */

    char line[512];
    if (!fgets(line, sizeof(line), f)) { fclose(f); return 0; }   /* header */

    int hit = 0;
    while (fgets(line, sizeof(line), f)) {
        char ifn[32];
        unsigned dest, gw, flags, refcnt, use, metric, mask;
        if (sscanf(line, "%31s %x %x %x %u %u %u %x",
                   ifn, &dest, &gw, &flags, &refcnt, &use, &metric, &mask) < 8)
            continue;
        if (!(flags & 0x1) || (flags & 0x2)) continue;   /* up + not a gateway route */
        if (strcmp(ifn, ifname) == 0) continue;          /* our own ipsec0 net */

        /* /proc stores dest/mask host-side little-endian; sscanf gave us the
         * value as-is. Compare on the wider of the two prefixes. */
        uint32_t lnet = __builtin_bswap32(dest);
        uint32_t lmask = __builtin_bswap32(mask);
        uint32_t rnet = u32(r->net);
        uint32_t rmask = mask_of(r->prefix);
        uint32_t m = lmask < rmask ? lmask : rmask;      /* shorter prefix = smaller mask */
        if ((lnet & m) == (rnet & m)) {
            snprintf(err, errcap,
                     "route %u.%u.%u.%u/%u collides with directly-connected net on %s "
                     "-- refusing (would hijack local/management traffic)",
                     r->net[0], r->net[1], r->net[2], r->net[3], (unsigned)r->prefix, ifn);
            hit = 1;
            break;
        }
    }
    fclose(f);
    return hit;
}

static int contains_ip(const wd_route *r, const uint8_t ip[4])
{
    uint32_t m = mask_of(r->prefix);
    return (u32(r->net) & m) == (u32(ip) & m);
}

int wd_routes_reconcile(wd_route_table *t, const char *ifname,
                        const wd_route *desired, size_t n_desired,
                        const uint8_t peer_ip[4],
                        char *err, size_t errcap)
{
    /* 1. Validate the WHOLE plan before touching the kernel. */
    for (size_t i = 0; i < n_desired; i++) {
        const wd_route *r = &desired[i];
        if (r->prefix == 0) {
            snprintf(err, errcap,
                     "peer offered 0.0.0.0/0 (full tunnel) -- unsupported in AP6, refusing");
            return -1;
        }
        if (contains_ip(r, peer_ip)) {
            snprintf(err, errcap,
                     "route %u.%u.%u.%u/%u contains the IKE gateway -- refusing (encapsulation loop)",
                     r->net[0], r->net[1], r->net[2], r->net[3], (unsigned)r->prefix);
            return -1;
        }
        if (collides_with_local(r, ifname, err, errcap)) return -1;
    }

    snprintf(t->ifname, sizeof(t->ifname), "%s", ifname);

    /* 2. Add every desired route not already owned; remember what we added so
     *    a later failure in THIS call rolls back only our own additions. */
    wd_route added[WD_ROUTE_MAX];
    size_t n_added = 0;

    for (size_t i = 0; i < n_desired; i++) {
        int owned = 0;
        for (size_t j = 0; j < t->n_owned; j++)
            if (same_route(&desired[i], &t->owned[j])) { owned = 1; break; }
        if (owned) continue;

        if (t->n_owned >= WD_ROUTE_OWNED_MAX) {
            snprintf(err, errcap, "too many tunnel routes");
            goto rollback;
        }
        if (wd_route_dev(ifname, desired[i].net, desired[i].prefix, 1, err, errcap) != 0)
            goto rollback;

        t->owned[t->n_owned++] = desired[i];
        added[n_added++] = desired[i];
    }

    /* 3. Remove owned routes that are no longer desired (AFTER the adds, so a
     *    net that stays permitted never has a routeless window across rekey). */
    for (size_t j = 0; j < t->n_owned; ) {
        int want = 0;
        for (size_t i = 0; i < n_desired; i++)
            if (same_route(&t->owned[j], &desired[i])) { want = 1; break; }
        if (want) { j++; continue; }
        char ignore[8];
        wd_route_dev(t->ifname, t->owned[j].net, t->owned[j].prefix, 0, ignore, sizeof(ignore));
        t->owned[j] = t->owned[--t->n_owned];
    }
    return 0;

rollback:
    for (size_t k = 0; k < n_added; k++) {
        char ignore[8];
        wd_route_dev(ifname, added[k].net, added[k].prefix, 0, ignore, sizeof(ignore));
        for (size_t j = 0; j < t->n_owned; j++)
            if (same_route(&added[k], &t->owned[j])) {
                t->owned[j] = t->owned[--t->n_owned];
                break;
            }
    }
    return -1;
}

void wd_routes_teardown(wd_route_table *t)
{
    for (size_t j = 0; j < t->n_owned; j++) {
        char ignore[8];
        wd_route_dev(t->ifname, t->owned[j].net, t->owned[j].prefix, 0, ignore, sizeof(ignore));
    }
    t->n_owned = 0;
}
