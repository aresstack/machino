/*
 * weirdiked -- the tunnel route manager (AP6).
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Owns EXACTLY the routes it installed over ipsec0, and nothing else. A
 * disconnect removes only owned routes; a foreign 192.168.178.0/24 that
 * happened to exist is never touched. Installation is transactional: on a
 * partial failure everything this call added is rolled back, so the box is
 * never left with half a tunnel.
 *
 * WeirdIKE knows nothing of this file -- it hands out selectors, the route
 * manager turns the NEGOTIATED ones into kernel routes.
 */
#ifndef WD_ROUTES_H
#define WD_ROUTES_H

#include <stdint.h>
#include <stddef.h>

#define WD_ROUTE_MAX 8          /* desired: TSr (<=4) + CP subnets (<=4) */
/* Owned can transiently exceed WD_ROUTE_MAX during a rekey that swaps the
 * whole set (add-before-remove): size it for the worst-case overlap. */
#define WD_ROUTE_OWNED_MAX (2 * WD_ROUTE_MAX)

enum { WD_SRC_TSR = 0, WD_SRC_CP = 1 };

typedef struct {
    uint8_t net[4];
    uint8_t prefix;             /* 0..32; 0 (== 0.0.0.0/0) is refused: no full tunnel in AP6 */
    uint8_t source;             /* WD_SRC_TSR | WD_SRC_CP */
} wd_route;

typedef struct {
    wd_route owned[WD_ROUTE_OWNED_MAX];
    size_t   n_owned;
    char     ifname[16];
} wd_route_table;

/* Bring the owned set in line with `desired` over `ifname`, new-before-old so
 * a still-permitted remote net never loses its route across a rekey:
 *   1. refuse the whole plan up front if any desired route is 0.0.0.0/0
 *      (full tunnel), contains `peer_ip` (encapsulation loop), or collides
 *      with a directly-connected local net (management hijack) -- err says
 *      which, nothing is touched;
 *   2. add every desired route not already owned; on any add failure roll
 *      back the adds from THIS call and return -1;
 *   3. remove owned routes no longer desired.
 * Idempotent: an unchanged plan performs no kernel operation. Returns 0/-1. */
int wd_routes_reconcile(wd_route_table *t, const char *ifname,
                        const wd_route *desired, size_t n_desired,
                        const uint8_t peer_ip[4],
                        char *err, size_t errcap);

/* Remove every owned route (disconnect). Idempotent; best-effort per route. */
void wd_routes_teardown(wd_route_table *t);

/* Test seam: the directly-connected-net check reads this instead of
 * /proc/net/route when set (path to a proc-net-route-format file). NULL =
 * the real /proc/net/route. */
void wd_routes_set_procfile(const char *path);

#endif /* WD_ROUTES_H */
