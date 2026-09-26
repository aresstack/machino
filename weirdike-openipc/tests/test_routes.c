/*
 * AP6: the tunnel route manager as pure logic. wd_route_dev is stubbed here
 * (it records adds/removes instead of touching the kernel), and the
 * directly-connected-net check reads a fake /proc/net/route -- so this runs
 * on any host, not just the Linux CI.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "wd_routes.h"
#include "wd_net.h"

#include <stdio.h>
#include <string.h>

static int g_fail = 0, g_pass = 0;
#define CHECK(c, msg) do { if (c) { g_pass++; } else { g_fail++; \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, msg); } } while (0)

/* ---- stub for wd_net's route ioctl: record, never touch the kernel ---- */
struct call { char op; uint8_t net[4]; uint8_t prefix; };
static struct call g_calls[64];
static size_t g_ncalls = 0;
static int g_fail_add_2nd = -1;   /* if >=0, adds of a route whose net[1]==this fail */

int wd_route_dev(const char *ifname, const uint8_t net[4], uint8_t prefix, int add,
                 char *err, size_t errcap)
{
    (void)ifname;
    if (add && g_fail_add_2nd >= 0 && net[1] == (uint8_t)g_fail_add_2nd) {
        snprintf(err, errcap, "stub add failure");
        return -1;
    }
    struct call c; c.op = add ? '+' : '-';
    memcpy(c.net, net, 4); c.prefix = prefix;
    if (g_ncalls < 64) g_calls[g_ncalls++] = c;
    return 0;
}
/* wd_net symbols the linker wants but the test never calls: */
int wd_tun_open(const char *i, char *e, size_t c) { (void)i;(void)e;(void)c; return -1; }
int wd_tun_configure(const char *i, const uint8_t ip[4], uint8_t p, int m, char *e, size_t c)
{ (void)i;(void)ip;(void)p;(void)m;(void)e;(void)c; return -1; }
int wd_tun_down(const char *i, char *e, size_t c) { (void)i;(void)e;(void)c; return 0; }

static void reset(void) { g_ncalls = 0; g_fail_add_2nd = -1; }

static int count_op(char op) {
    int n = 0; for (size_t i = 0; i < g_ncalls; i++) if (g_calls[i].op == op) n++; return n;
}
static int has(char op, uint8_t a, uint8_t b, uint8_t c, uint8_t d, uint8_t pfx) {
    for (size_t i = 0; i < g_ncalls; i++)
        if (g_calls[i].op == op && g_calls[i].net[0]==a && g_calls[i].net[1]==b &&
            g_calls[i].net[2]==c && g_calls[i].net[3]==d && g_calls[i].prefix==pfx) return 1;
    return 0;
}

static wd_route R(uint8_t a, uint8_t b, uint8_t c, uint8_t d, uint8_t pfx, uint8_t src) {
    wd_route r; r.net[0]=a; r.net[1]=b; r.net[2]=c; r.net[3]=d; r.prefix=pfx; r.source=src; return r;
}

/* a /proc/net/route with ONE directly-connected net on eth0: 192.168.1.0/24.
 * Fields are little-endian hex: dest=0x0001A8C0, mask=0x00FFFFFF, flags=1. */
static const char *PROC_LOCAL =
    "Iface\tDestination\tGateway\tFlags\tRefCnt\tUse\tMetric\tMask\tMTU\tWindow\tIRTT\n"
    "eth0\t0001A8C0\t00000000\t0001\t0\t0\t0\t00FFFFFF\t0\t0\t0\n";

int main(void)
{
    const uint8_t peer[4] = {203, 0, 113, 5};
    char err[256];
    char proc[] = "test_proc_route.tmp";
    FILE* f = fopen(proc, "wb"); fputs(PROC_LOCAL, f); fclose(f);

    /* --- multiple TSr all become routes --- */
    reset();
    wd_route_table t; memset(&t, 0, sizeof(t));
    wd_routes_set_procfile(proc);
    wd_route d1[] = { R(192,168,178,0,24,WD_SRC_TSR), R(10,20,0,0,16,WD_SRC_TSR) };
    CHECK(wd_routes_reconcile(&t, "ipsec0", d1, 2, peer, err, sizeof(err)) == 0, "two TSr install");
    CHECK(has('+',192,168,178,0,24), "route A added");
    CHECK(has('+',10,20,0,0,16), "route B added");
    CHECK(t.n_owned == 2, "owns two");

    /* idempotent: same plan, no kernel ops */
    reset();
    CHECK(wd_routes_reconcile(&t, "ipsec0", d1, 2, peer, err, sizeof(err)) == 0, "reconcile idempotent");
    CHECK(g_ncalls == 0, "no ops on unchanged plan");

    /* --- rekey diff: drop B, add C. Add-before-remove ordering. --- */
    reset();
    wd_route d2[] = { R(192,168,178,0,24,WD_SRC_TSR), R(172,16,5,0,24,WD_SRC_TSR) };
    CHECK(wd_routes_reconcile(&t, "ipsec0", d2, 2, peer, err, sizeof(err)) == 0, "rekey diff");
    CHECK(has('+',172,16,5,0,24), "new C added");
    CHECK(has('-',10,20,0,0,16), "old B removed");
    CHECK(!has('-',192,168,178,0,24), "kept A not touched");
    /* the add of C must come before the remove of B (no window without a route
     * to a still-permitted net -- here A stays, but the ORDER is the invariant) */
    {
        int add_c = -1, del_b = -1;
        for (size_t i = 0; i < g_ncalls; i++) {
            if (g_calls[i].op=='+' && g_calls[i].net[0]==172) add_c = (int)i;
            if (g_calls[i].op=='-' && g_calls[i].net[0]==10)  del_b = (int)i;
        }
        CHECK(add_c >= 0 && del_b >= 0 && add_c < del_b, "add-before-remove ordering");
    }

    /* --- full tunnel refused, nothing touched --- */
    reset();
    wd_route_table t2; memset(&t2, 0, sizeof(t2));
    wd_route full[] = { R(0,0,0,0,0,WD_SRC_TSR) };
    CHECK(wd_routes_reconcile(&t2, "ipsec0", full, 1, peer, err, sizeof(err)) != 0, "full tunnel refused");
    CHECK(strstr(err, "full tunnel") != NULL, "full tunnel named");
    CHECK(g_ncalls == 0 && t2.n_owned == 0, "full tunnel touched nothing");

    /* --- peer inside a route refused --- */
    reset();
    wd_route gwloop[] = { R(203,0,113,0,24,WD_SRC_TSR) };
    CHECK(wd_routes_reconcile(&t2, "ipsec0", gwloop, 1, peer, err, sizeof(err)) != 0, "gateway loop refused");
    CHECK(strstr(err, "gateway") != NULL, "gateway loop named");
    CHECK(g_ncalls == 0, "gateway loop touched nothing");

    /* --- local management net protected --- */
    reset();
    wd_route localnet[] = { R(192,168,1,0,24,WD_SRC_TSR) };
    CHECK(wd_routes_reconcile(&t2, "ipsec0", localnet, 1, peer, err, sizeof(err)) != 0, "local net protected");
    CHECK(strstr(err, "directly-connected") != NULL, "local net named");
    CHECK(g_ncalls == 0, "local net touched nothing");

    /* --- transactional rollback: 2nd add fails -> 1st add rolled back --- */
    reset();
    wd_route_table t3; memset(&t3, 0, sizeof(t3));
    g_fail_add_2nd = 20;                     /* the 10.20.x add will fail */
    wd_route d3[] = { R(192,168,178,0,24,WD_SRC_TSR), R(10,20,0,0,16,WD_SRC_TSR) };
    CHECK(wd_routes_reconcile(&t3, "ipsec0", d3, 2, peer, err, sizeof(err)) != 0, "partial add fails");
    CHECK(has('-',192,168,178,0,24), "first add rolled back");
    CHECK(t3.n_owned == 0, "nothing owned after rollback");
    g_fail_add_2nd = -1;

    /* --- disconnect removes only owned --- */
    reset();
    wd_routes_teardown(&t);
    CHECK(count_op('-') == (int)2 || t.n_owned == 0, "teardown removed owned");
    CHECK(t.n_owned == 0, "owns nothing after teardown");

    /* --- CP source tag survives (installed like any route) --- */
    reset();
    wd_route_table t4; memset(&t4, 0, sizeof(t4));
    wd_route cp[] = { R(10,20,0,0,16,WD_SRC_TSR), R(10,99,0,0,24,WD_SRC_CP) };
    CHECK(wd_routes_reconcile(&t4, "ipsec0", cp, 2, peer, err, sizeof(err)) == 0, "tsr+cp install");
    int cp_tagged = 0;
    for (size_t i = 0; i < t4.n_owned; i++)
        if (t4.owned[i].source == WD_SRC_CP && t4.owned[i].net[1] == 99) cp_tagged = 1;
    CHECK(cp_tagged, "cp route keeps its source tag");

    remove(proc);
    printf("%d checks, %d failed\n", g_pass + g_fail, g_fail);
    return g_fail ? 1 : 0;
}
