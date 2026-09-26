/*
 * weirdiked -- IKEv2/IPsec client daemon for OpenIPC on Ingenic T40.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Why this exists as its own process
 * ----------------------------------
 * The camera has no kernel IPsec: /proc/net/xfrm_stat does not exist and
 * /proc/net/protocols lists neither ESP nor AH. The only possible data path is
 * therefore userspace ESP over a TUN device -- which is exactly what WeirdIKE
 * provides. Machino (the media daemon) is deliberately NOT involved: a wedged
 * VPN must never be able to stall the video pipeline, and the two have
 * separate lifecycles.
 *
 * Shape of the loop
 * -----------------
 * One poll() over four kinds of fd:
 *
 *   udp/500, udp/4500   IKE, and -- once NAT-T is up -- UDP-encapsulated ESP.
 *                       Inbound 4500 traffic is demultiplexed with
 *                       natt_classify(): IKE goes to the core, ESP to the data
 *                       path, a single 0xFF is a keepalive and is dropped.
 *   tun                 outbound plaintext IP packets -> esp_session_seal()
 *   control socket      weirdikectl
 *
 * The core never reads a socket itself: its transport vtable has a send() and
 * a recv() that always reports "nothing pending". That is the same split the
 * ESP32 integration uses, and it keeps all I/O in one place.
 *
 * Scope, stated plainly
 * ---------------------
 * UDP-encapsulated ESP (RFC 3948, port 4500) only. Bare ESP (IP protocol 50)
 * would need a raw socket and separate receive handling; it is not built. A
 * camera behind NAT -- the case this was written for -- always ends up on 4500
 * anyway. If the peer negotiates no NAT-T, the daemon says so and stops rather
 * than pretending to carry traffic.
 */
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <arpa/inet.h>
#include <netinet/in.h>

#include "weirdike.h"
#include "ike_natt.h"
#include "esp.h"
#include "esp_session.h"
#include "crypto_mbedtls.h"

#include "wd_config.h"
#include "wd_net.h"
#include "wd_routes.h"

#define WD_CTL_PATH   "/var/run/weirdike.sock"
#define WD_CONF_PATH  "/etc/weirdike/weirdike.conf"
#define WD_MAX_DGRAM  2048

/* ------------------------------------------------------------------ logging */

static int g_foreground = 0;

static void wd_log(int level, const char *fmt, ...)
{
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);

    if (g_foreground) fprintf(stderr, "weirdiked: %s\n", line);
    else              syslog(level, "%s", line);
}

/* The core logs through this. Levels are its own; nothing it emits contains
 * key material -- WeirdIKE's wire summaries are deliberately secrets-free. */
static void core_log(void *ctx, int lvl, const char *msg)
{
    (void)ctx;
    wd_log(lvl > 1 ? LOG_DEBUG : LOG_INFO, "ike: %s", msg ? msg : "");
}

/* ---------------------------------------------------------------- transport */

typedef struct {
    int      fd500;
    int      fd4500;
    uint16_t active_port;      /* which local port the core last opened */
    uint8_t  src_ip[4];        /* concrete source address; NAT-D must not see 0.0.0.0 */
    uint8_t  peer_ip[4];
    int      have_src;
} wd_transport;

static int tr_open(void *ctx, uint16_t local_port)
{
    wd_transport *t = ctx;
    /* Both sockets are bound before the core starts; "open" only records which
     * one it considers current. The NAT-T migration to 4500 therefore cannot
     * fail halfway and leave us without a socket. */
    if (local_port != 500 && local_port != 4500) return -1;
    t->active_port = local_port;
    return 0;
}

static int tr_resolve(void *ctx, const char *host, weirdike_endpoint_t *out)
{
    wd_transport *t = ctx;
    uint8_t ip[4];
    if (wd_resolve4(host, ip) != 0) return -1;
    memset(out, 0, sizeof(*out));
    memcpy(out->ip, ip, 4);
    memcpy(t->peer_ip, ip, 4);

    if (!t->have_src && wd_source_addr_for(ip, t->src_ip) == 0) t->have_src = 1;
    return 0;
}

static int tr_local(void *ctx, weirdike_endpoint_t *out)
{
    wd_transport *t = ctx;
    if (!t->have_src) return -1;
    memset(out, 0, sizeof(*out));
    memcpy(out->ip, t->src_ip, 4);
    out->port = t->active_port ? t->active_port : 500;
    return 0;
}

static int tr_send(void *ctx, const weirdike_endpoint_t *dst, const uint8_t *data, size_t len)
{
    wd_transport *t = ctx;
    int fd = (dst->port == 4500) ? t->fd4500 : t->fd500;

    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port   = htons(dst->port);
    memcpy(&a.sin_addr, dst->ip, 4);

    ssize_t n = sendto(fd, data, len, 0, (struct sockaddr *)&a, sizeof(a));
    return (n == (ssize_t)len) ? 0 : -1;
}

/* The core never pulls: the daemon pushes with weirdike_input_datagram(). */
static int tr_recv(void *ctx, weirdike_endpoint_t *src, uint8_t *buf, size_t cap, int timeout_ms)
{
    (void)ctx; (void)src; (void)buf; (void)cap; (void)timeout_ms;
    return 0;
}

static void tr_close(void *ctx) { (void)ctx; }

/* --------------------------------------------------------------- daemon state */

typedef struct {
    wd_config        cfg;
    wd_transport     tr;
    weirdike_ctx    *ike;
    weirdike_crypto_t      crypto;
    weirdike_transport_t   vt;
    weirdike_platform_t    plat;
    weirdike_mbedtls_ctx   mbed;

    int              tun_fd;
    int              ctl_fd;

    esp_session_t    esp;
    int              esp_up;
    uint32_t         esp_generation;   /* child generation the session was built from */
    uint8_t         *esp_scratch;

    uint64_t         tx_packets, rx_packets;
    uint64_t         tx_bytes,   rx_bytes;
    uint32_t         started_ms;
    int              want_stop;

    /* AP4/AP6: data path wiring. The CURRENT (narrowed) selectors gate TX and
     * RX; the route table (AP6) mirrors ALL negotiated remote nets + CP
     * subnets and owns exactly them. Dropped-by-selector counters are visible
     * via ctl status so a misrouted client shows up as numbers, not silence. */
    weirdike_child_sa_t child;
    int              have_child;
    int              tun_configured;
    wd_route_table   routes;
    int              full_tunnel_refused;   /* peer offered 0.0.0.0/0 (status flag) */
    uint64_t         tx_drop_sel, rx_drop_sel;
} wd_daemon;

/* Conservative TUN MTU: 1500 minus the ESP-in-UDP worst case for the AP2
 * suite (outer IP 20 + UDP 8 + SPI/seq 8 + CBC-IV 16 + padding <=17 + ICV 16
 * = 85 -> 1415 usable), rounded DOWN so a PPPoE/underlay with its own
 * overhead still never fragments the outer packet. */
#define WD_TUN_MTU 1400

static volatile sig_atomic_t g_signal = 0;
static void on_signal(int s) { g_signal = s; }

/* ------------------------------------------------------------------ data path */

/* v4 membership in one selector. The core's TS model carries ranges; for
 * IPv4 they live in bytes 0..3. Ports/protocol stay unchecked here -- M1
 * negotiates proto 0 and the full port range, and narrowing below that
 * would have to come from the SELECTORS, not from an opinion of ours. */
static int ts_contains_v4(const weirdike_ts_t *ts, const uint8_t ip[4])
{
    if (ts->address_family != 4) return 0;
    return memcmp(ip, ts->start_addr, 4) >= 0 && memcmp(ip, ts->end_addr, 4) <= 0;
}

/* Membership in the full narrowed TSr set (remote_ts_list; remote_ts is
 * entry 0, kept as fallback for a core that reports n_remote_ts == 0). */
static int child_remote_contains(const weirdike_child_sa_t *c, const uint8_t ip[4])
{
    if (c->n_remote_ts == 0) return ts_contains_v4(&c->remote_ts, ip);
    for (size_t i = 0; i < c->n_remote_ts; i++)
        if (ts_contains_v4(&c->remote_ts_list[i], ip)) return 1;
    return 0;
}

/* Is [lo,hi] entirely inside one accepted TSr? A CP subnet may only become a
 * route if the WHOLE block sits within a negotiated selector -- checking just
 * its base would let a /8 CP whose base falls in a /24 TSr drag traffic to
 * millions of addresses the SA never covered (AP6 §6). */
static int child_remote_covers_range(const weirdike_child_sa_t *c,
                                     const uint8_t lo[4], const uint8_t hi[4])
{
    size_t n = c->n_remote_ts ? c->n_remote_ts : 1;
    for (size_t i = 0; i < n; i++) {
        const weirdike_ts_t *ts = c->n_remote_ts ? &c->remote_ts_list[i] : &c->remote_ts;
        if (ts->address_family != 4) continue;
        if (memcmp(lo, ts->start_addr, 4) >= 0 && memcmp(hi, ts->end_addr, 4) <= 0) return 1;
    }
    return 0;
}

/* start..end -> net/prefix, when the range IS a clean CIDR block (aligned
 * power-of-two). Responders narrow to subnets in practice; a ragged range
 * cannot be expressed as one kernel route and is reported instead. */
static int ts_range_to_cidr(const weirdike_ts_t *ts, uint8_t net[4], uint8_t *prefix)
{
    if (ts->address_family != 4) return -1;
    uint32_t s = ((uint32_t)ts->start_addr[0] << 24) | ((uint32_t)ts->start_addr[1] << 16)
               | ((uint32_t)ts->start_addr[2] << 8)  |  (uint32_t)ts->start_addr[3];
    uint32_t e = ((uint32_t)ts->end_addr[0] << 24) | ((uint32_t)ts->end_addr[1] << 16)
               | ((uint32_t)ts->end_addr[2] << 8)  |  (uint32_t)ts->end_addr[3];
    if (e < s) return -1;
    uint32_t span = e - s;                       /* size-1: all-ones for a block */
    if ((span & (span + 1)) != 0) return -1;     /* not 2^k - 1 */
    if ((s & span) != 0) return -1;              /* not aligned */
    uint8_t p = 32;
    while (span) { span >>= 1; p--; }
    memcpy(net, ts->start_addr, 4);
    *prefix = p;
    return 0;
}

/* Collect the desired route set from the NEGOTIATED selectors (all of TSr,
 * AP6 §5) plus any CP subnets the gateway assigned (AP6 §6, tagged apart).
 * A TSr entry that is not a clean CIDR block is reported and skipped -- it
 * cannot be one kernel route. Returns the count. */
static size_t collect_routes(const wd_daemon *d, wd_route *out, size_t cap,
                             int *full_tunnel)
{
    *full_tunnel = 0;
    size_t n = 0;
    const weirdike_child_sa_t *c = &d->child;

    size_t n_ts = c->n_remote_ts ? c->n_remote_ts : 1;
    for (size_t i = 0; i < n_ts && n < cap; i++) {
        const weirdike_ts_t *ts = c->n_remote_ts ? &c->remote_ts_list[i] : &c->remote_ts;
        uint8_t net[4], prefix;
        if (ts_range_to_cidr(ts, net, &prefix) != 0) {
            wd_log(LOG_ERR, "data path: negotiated TSr #%u is not a CIDR block -- skipped",
                   (unsigned)i);
            continue;
        }
        if (prefix == 0) { *full_tunnel = 1; continue; }   /* 0.0.0.0/0: rejected downstream */
        memcpy(out[n].net, net, 4); out[n].prefix = prefix; out[n].source = WD_SRC_TSR;
        n++;
    }

    /* CP subnets: additional routing info from the gateway. Only installed
     * when they sit INSIDE an accepted TSr -- a CP net may never drag traffic
     * outside the cryptographically negotiated selector into ipsec0 (AP6 §6). */
    weirdike_cp_t cp;
    if (weirdike_get_cp(d->ike, &cp) == 0) {
        for (size_t i = 0; i < cp.subnet_count && n < cap; i++) {
            uint8_t pfx = 0;
            uint32_t m = ((uint32_t)cp.subnet_mask[i][0] << 24) | ((uint32_t)cp.subnet_mask[i][1] << 16)
                       | ((uint32_t)cp.subnet_mask[i][2] << 8)  |  (uint32_t)cp.subnet_mask[i][3];
            for (int b = 31; b >= 0; b--) { if (m & (1u << b)) pfx++; else break; }
            if (pfx == 0) continue;
            /* consistency: the WHOLE CP block (base..broadcast) must fall
             * inside an accepted TSr -- not just its base (AP6 §6). */
            uint32_t base = (((uint32_t)cp.subnet[i][0] << 24) | ((uint32_t)cp.subnet[i][1] << 16)
                          | ((uint32_t)cp.subnet[i][2] << 8) | cp.subnet[i][3]) & m;
            uint32_t bcast = base | ~m;
            uint8_t lo[4] = { (uint8_t)(base>>24), (uint8_t)(base>>16), (uint8_t)(base>>8), (uint8_t)base };
            uint8_t hi[4] = { (uint8_t)(bcast>>24), (uint8_t)(bcast>>16), (uint8_t)(bcast>>8), (uint8_t)bcast };
            if (!child_remote_covers_range(c, lo, hi)) {
                wd_log(LOG_WARNING, "CP subnet %u.%u.%u.%u/%u not fully within negotiated TSr -- ignored",
                       cp.subnet[i][0], cp.subnet[i][1], cp.subnet[i][2], cp.subnet[i][3],
                       (unsigned)pfx);
                continue;
            }
            memcpy(out[n].net, lo, 4); out[n].prefix = pfx; out[n].source = WD_SRC_CP;
            n++;
        }
    }
    return n;
}

/* Configure ipsec0 and reconcile ALL tunnel routes (AP6). Re-entrant: a rekey
 * that changes the TSr set produces a route diff (add-before-remove) inside
 * the route manager. Full tunnel (0.0.0.0/0) is refused with a status flag;
 * the gateway/local-net guards live in the route manager. */
static void data_path_install(wd_daemon *d)
{
    char err[256];

    if (!d->tun_configured) {
        /* The inner address is the local_ts the peer actually agreed to. */
        if (d->child.local_ts.address_family != 4) {
            wd_log(LOG_ERR, "data path: non-IPv4 local selector -- not configuring %s", d->cfg.ifname);
            return;
        }
        if (wd_tun_configure(d->cfg.ifname, d->child.local_ts.start_addr,
                             d->cfg.have_local_ts ? d->cfg.local_ts.prefix : 32,
                             WD_TUN_MTU, err, sizeof(err)) != 0) {
            wd_log(LOG_ERR, "data path: %s", err);
            return;
        }
        d->tun_configured = 1;
        wd_log(LOG_NOTICE, "%s configured (mtu %d)", d->cfg.ifname, WD_TUN_MTU);
    }

    wd_route desired[WD_ROUTE_MAX];
    size_t n = collect_routes(d, desired, WD_ROUTE_MAX, &d->full_tunnel_refused);
    if (d->full_tunnel_refused)
        wd_log(LOG_ERR, "data path: peer requested 0.0.0.0/0 (full tunnel) -- unsupported (AP6)");
    if (n == 0) {
        wd_log(LOG_ERR, "data path: no installable remote net -- no tunnel route");
        return;
    }

    if (wd_routes_reconcile(&d->routes, d->cfg.ifname, desired, n, d->tr.peer_ip,
                            err, sizeof(err)) != 0) {
        wd_log(LOG_ERR, "data path: %s", err);
        return;
    }
    wd_log(LOG_NOTICE, "%u tunnel route(s) over %s", (unsigned)d->routes.n_owned, d->cfg.ifname);
}

/* AP7 §3/§8: fail closed. On a lost tunnel (DPD gave up -> FAILED, or a
 * hard stop) the data path must come DOWN, not linger: routes withdrawn,
 * ipsec0 down, ESP session zeroized. Every step idempotent, so this is safe
 * to call from the FAILED transition AND the exit path. No traffic may ride a
 * half-dead SA. */
static void data_path_teardown(wd_daemon *d)
{
    char err[128];
    wd_routes_teardown(&d->routes);
    if (d->tun_configured) { wd_tun_down(d->cfg.ifname, err, sizeof(err)); d->tun_configured = 0; }
    if (d->esp_up) { esp_session_deinit(&d->esp); d->esp_up = 0; }
    d->have_child = 0;
    d->esp_generation = 0;
}

/* A Child SA was (re)negotiated: rebuild the ESP session on the new keys. */
static void esp_refresh(wd_daemon *d)
{
    weirdike_child_sa_t child;
    if (weirdike_get_child_sa(d->ike, &child) != 0) return;

    uint32_t gen = weirdike_child_generation(d->ike);
    if (d->esp_up && gen == d->esp_generation) return;

    if (d->esp_up) { esp_session_deinit(&d->esp); d->esp_up = 0; }

    if (esp_session_init(&d->esp, &child, &d->crypto, d->esp_scratch, ESP_MAX_PACKET) != 0) {
        wd_log(LOG_ERR, "child SA %u: ESP session refused (unsupported suite?)", (unsigned)gen);
        return;
    }
    d->esp_up = 1;
    d->esp_generation = gen;
    d->child = child;
    d->have_child = 1;
    wd_log(LOG_NOTICE, "child SA %u installed, data path up", (unsigned)gen);
    data_path_install(d);                        /* AP4: tun + split route follow the SA */
}

/* TUN -> ESP -> UDP/4500 */
static void pump_tun(wd_daemon *d)
{
    uint8_t plain[ESP_MAX_PACKET];
    uint8_t packet[ESP_MAX_PACKET];

    for (int i = 0; i < 16; i++) {           /* bounded: never starve the other fds */
        ssize_t n = read(d->tun_fd, plain, sizeof(plain));
        if (n <= 0) return;
        if (!d->esp_up) continue;            /* no SA yet: drop, never send it in the clear */

        /* AP4: only traffic the NEGOTIATED selectors cover may enter the
         * tunnel -- src must sit in local_ts, dst in the narrowed TSr set.
         * Anything else (IPv6, stray daemons, a widened route) is dropped
         * and counted, never encrypted "because it happened to arrive". */
        if (n < 20 || (plain[0] >> 4) != 4 || !d->have_child ||
            !ts_contains_v4(&d->child.local_ts, plain + 12) ||
            !child_remote_contains(&d->child, plain + 16)) {
            d->tx_drop_sel++;
            continue;
        }

        size_t out_len = 0;
        int rc = esp_session_seal(&d->esp, 4 /* IPv4 */, plain, (size_t)n,
                                  packet, sizeof(packet), &out_len);
        if (rc == ESP_SESSION_EXHAUSTED) {
            wd_log(LOG_WARNING, "ESP sequence space exhausted -- forcing a child rekey");
            weirdike_rekey_child(d->ike, wd_now_ms());
            return;
        }
        if (rc != 0) continue;

        weirdike_endpoint_t dst;
        memset(&dst, 0, sizeof(dst));
        memcpy(dst.ip, d->tr.peer_ip, 4);
        dst.port = 4500;
        if (tr_send(&d->tr, &dst, packet, out_len) == 0) {
            d->tx_packets++;
            d->tx_bytes += (uint64_t)out_len;
            weirdike_child_traffic(d->ike, (uint32_t)out_len);
        }
    }
}

/* One inbound UDP datagram from port 4500. `from` is the OBSERVED packet
 * source -- the core needs it for NAT-D verification (roter Lauf 2: with
 * from=NULL the core must abort NAT-T with "no observed source endpoint"). */
static void on_udp4500(wd_daemon *d, const uint8_t *dg, size_t len,
                       const weirdike_endpoint_t *from)
{
    size_t off = 0;
    switch (natt_classify(dg, len, &off)) {
    case NATT_DATAGRAM_IKE:
        /* Hand over the datagram as received, marker included: the core
         * classifies it again and strips the marker itself. */
        weirdike_input_datagram(d->ike, dg, len, from);
        break;

    case NATT_DATAGRAM_ESP: {
        if (!d->esp_up) return;
        uint8_t plain[ESP_MAX_PACKET];
        uint8_t next_header = 0;
        size_t  plain_len = 0;
        if (esp_session_open(&d->esp, dg, len, &next_header,
                             plain, sizeof(plain), &plain_len) != 0) {
            return;                         /* replay, bad ICV, wrong SA -- silently dropped */
        }
        if (next_header != 4) return;       /* only IPv4 rides this tunnel */
        /* AP4: mirror of the TX gate -- a decrypting gateway could still
         * send us packets OUTSIDE the negotiated selectors (src not in TSr,
         * dst not our local_ts). Those never reach the TUN device. */
        if (plain_len < 20 || (plain[0] >> 4) != 4 || !d->have_child ||
            !child_remote_contains(&d->child, plain + 12) ||
            !ts_contains_v4(&d->child.local_ts, plain + 16)) {
            d->rx_drop_sel++;
            return;
        }
        if (write(d->tun_fd, plain, plain_len) == (ssize_t)plain_len) {
            d->rx_packets++;
            d->rx_bytes += (uint64_t)plain_len;
            weirdike_child_traffic(d->ike, (uint32_t)len);
        }
        break;
    }

    case NATT_DATAGRAM_KEEPALIVE:
        break;                              /* nothing to do; presence is the point */

    case NATT_DATAGRAM_MALFORMED:
    default:
        break;
    }
}

/* ---------------------------------------------------------------- control */

static int ctl_listen(char *err, size_t errcap)
{
    unlink(WD_CTL_PATH);

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { snprintf(err, errcap, "control socket: %s", strerror(errno)); return -1; }

    struct sockaddr_un a;
    memset(&a, 0, sizeof(a));
    a.sun_family = AF_UNIX;
    snprintf(a.sun_path, sizeof(a.sun_path), "%s", WD_CTL_PATH);

    /* 0600 before anyone can connect: the status output names the gateway and
     * the negotiated suite, and `down` would drop the tunnel. Neither belongs
     * to every process on the box. */
    mode_t old = umask(0177);
    int rc = bind(fd, (struct sockaddr *)&a, sizeof(a));
    umask(old);
    if (rc < 0) { snprintf(err, errcap, "bind control socket: %s", strerror(errno)); close(fd); return -1; }

    if (chmod(WD_CTL_PATH, 0600) != 0 || listen(fd, 4) != 0) {
        snprintf(err, errcap, "control socket setup: %s", strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

static const char *state_name(wd_daemon *d)
{
    return weirdike_state_str(weirdike_state(d->ike));
}

static void ctl_status(wd_daemon *d, char *buf, size_t cap)
{
    weirdike_diag_t dg;
    int have = (weirdike_get_diag(d->ike, &dg) == 0);

    char lts[24], rts[24];
    wd_cidr_str(d->cfg.have_local_ts  ? &d->cfg.local_ts  : NULL, lts, sizeof(lts));
    wd_cidr_str(d->cfg.have_remote_ts ? &d->cfg.remote_ts : NULL, rts, sizeof(rts));

    uint32_t up_s = (wd_now_ms() - d->started_ms) / 1000u;

    /* Deliberately no PSK, no key material, no EAP identity. */
    snprintf(buf, cap,
             "state=%s\n"
             "gateway=%s:%u\n"
             "interface=%s\n"
             "local_ts=%s\n"
             "remote_ts=%s\n"
             "natt=%s\n"
             "nat_detected=%s\n"
             "child=%s\n"
             "child_generation=%u\n"
             "ike_generation=%u\n"
             "last_notify=%u\n"
             "uptime_s=%u\n"
             "tx_packets=%llu\n"
             "tx_bytes=%llu\n"
             "rx_packets=%llu\n"
             "rx_bytes=%llu\n"
             "tx_drop_selector=%llu\n"
             "rx_drop_selector=%llu\n"
             /* AP5: transport facts, MEASURED not assumed. ike follows the
              * port the core actually floated to; ESP is always ESP-in-UDP
              * on this daemon (NAT-T-only, PLATFORM.md) -- saying so here
              * keeps the status honest instead of implying raw ESP. */
             "ike_transport=%s\n"
             "esp_transport=udp4500\n"
             /* AP6: full-tunnel refusal is a fact the UI must see, not a
              * silent drop. */
             "full_tunnel_refused=%s\n",
             state_name(d),
             d->cfg.gateway, (unsigned)d->cfg.port,
             d->cfg.ifname,
             lts, rts,
             have && dg.use_natt ? "yes" : "no",
             have && dg.nat_detected ? "yes" : "no",
             d->esp_up ? "up" : "down",
             (unsigned)(d->esp_up ? d->esp_generation : 0),
             (unsigned)(have ? dg.ike_generation : 0),
             (unsigned)(have ? dg.last_notify : 0),
             (unsigned)up_s,
             (unsigned long long)d->tx_packets,
             (unsigned long long)d->tx_bytes,
             (unsigned long long)d->rx_packets,
             (unsigned long long)d->rx_bytes,
             (unsigned long long)d->tx_drop_sel,
             (unsigned long long)d->rx_drop_sel,
             d->tr.active_port == 4500 ? "udp4500" : "udp500",
             d->full_tunnel_refused ? "yes" : "no");

    /* AP6 §12: one "route=" line per owned route, tagged with its source, so
     * the status carries the INSTALLED truth (not the requested config). */
    for (size_t i = 0; i < d->routes.n_owned; i++) {
        const wd_route *r = &d->routes.owned[i];
        char line[64];
        snprintf(line, sizeof(line), "route=%u.%u.%u.%u/%u %s %s\n",
                 r->net[0], r->net[1], r->net[2], r->net[3], (unsigned)r->prefix,
                 r->source == WD_SRC_CP ? "cp" : "tsr", d->cfg.ifname);
        size_t have_len = strlen(buf), add = strlen(line);
        if (have_len + add < cap) memcpy(buf + have_len, line, add + 1);
    }
}

static void ctl_serve(wd_daemon *d)
{
    int c = accept(d->ctl_fd, NULL, NULL);
    if (c < 0) return;

    char req[64];
    ssize_t n = read(c, req, sizeof(req) - 1);
    if (n <= 0) { close(c); return; }
    req[n] = 0;
    while (n > 0 && (req[n - 1] == '\n' || req[n - 1] == '\r')) req[--n] = 0;

    char out[1024];
    if (!strcmp(req, "status")) {
        ctl_status(d, out, sizeof(out));
    } else if (!strcmp(req, "down")) {
        weirdike_disconnect(d->ike, wd_now_ms());
        d->want_stop = 1;
        snprintf(out, sizeof(out), "ok: disconnecting\n");
    } else if (!strcmp(req, "rekey")) {
        int rc = weirdike_rekey_child(d->ike, wd_now_ms());
        snprintf(out, sizeof(out), "%s\n", rc == 0 ? "ok: child rekey requested" : "error: refused");
    } else {
        snprintf(out, sizeof(out), "error: unknown command\n");
    }

    ssize_t ignored = write(c, out, strlen(out));
    (void)ignored;
    close(c);
}

/* ------------------------------------------------------------------- startup */

static int load_config(const char *path, wd_config *cfg, char *err, size_t errcap)
{
    struct stat st;
    if (stat(path, &st) != 0) { snprintf(err, errcap, "cannot stat %s: %s", path, strerror(errno)); return -1; }

    /* The file holds a pre-shared key. Refuse to read it if it is readable by
     * anyone else -- silently carrying on would make the mode meaningless. */
    if (st.st_mode & (S_IRWXG | S_IRWXO)) {
        snprintf(err, errcap, "%s is group/world accessible (mode %o) -- refusing; chmod 600 it",
                 path, (unsigned)(st.st_mode & 07777));
        return -1;
    }

    FILE *f = fopen(path, "rb");
    if (!f) { snprintf(err, errcap, "cannot open %s: %s", path, strerror(errno)); return -1; }

    static char text[8192];
    size_t n = fread(text, 1, sizeof(text) - 1, f);
    int too_big = !feof(f);
    fclose(f);
    if (too_big) { snprintf(err, errcap, "%s is larger than 8 KiB", path); return -1; }
    text[n] = 0;

    int rc = wd_config_parse(text, n, cfg, err, errcap);
    memset(text, 0, sizeof(text));          /* the PSK was in here */
    return rc;
}

/* WeirdIKE deliberately offers no CIDR helper -- "hosts that need a specific
 * selector parse a.b.c.d/prefix themselves and fill weirdike_ts_t". So we do:
 * an IKEv2 selector is an inclusive address RANGE, and a prefix is just a
 * range whose host bits are all 0 at the start and all 1 at the end. */
static void ts_from_cidr(weirdike_ts_t *ts, const wd_cidr *c)
{
    memset(ts, 0, sizeof(*ts));
    ts->address_family = 4;
    ts->ip_protocol    = 0;          /* any */
    ts->start_port     = 0;
    ts->end_port       = 0xFFFF;

    uint32_t addr = ((uint32_t)c->ip[0] << 24) | ((uint32_t)c->ip[1] << 16) |
                    ((uint32_t)c->ip[2] << 8)  |  (uint32_t)c->ip[3];
    uint32_t mask = (c->prefix == 0) ? 0u : (0xFFFFFFFFu << (32 - c->prefix));
    uint32_t lo   = addr & mask;
    uint32_t hi   = lo | ~mask;

    ts->start_addr[0] = (uint8_t)(lo >> 24); ts->start_addr[1] = (uint8_t)(lo >> 16);
    ts->start_addr[2] = (uint8_t)(lo >> 8);  ts->start_addr[3] = (uint8_t)lo;
    ts->end_addr[0]   = (uint8_t)(hi >> 24); ts->end_addr[1]   = (uint8_t)(hi >> 16);
    ts->end_addr[2]   = (uint8_t)(hi >> 8);  ts->end_addr[3]   = (uint8_t)hi;
}

int main(int argc, char **argv)
{
    const char *conf = WD_CONF_PATH;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-f"))                   g_foreground = 1;
        else if (!strcmp(argv[i], "-c") && i + 1 < argc) conf = argv[++i];
        else {
            fprintf(stderr, "usage: weirdiked [-f] [-c /etc/weirdike/weirdike.conf]\n");
            return 2;
        }
    }

    if (!g_foreground) openlog("weirdiked", LOG_PID, LOG_DAEMON);

    static wd_daemon d;
    memset(&d, 0, sizeof(d));
    d.tun_fd = d.ctl_fd = -1;

    char err[256] = {0};
    if (load_config(conf, &d.cfg, err, sizeof(err)) != 0) {
        wd_log(LOG_ERR, "config: %s", err);
        return 1;
    }
    wd_log(LOG_NOTICE, "starting: gateway=%s:%u interface=%s nat_t=%s",
           d.cfg.gateway, (unsigned)d.cfg.port, d.cfg.ifname, d.cfg.nat_t ? "yes" : "no");

    /* AP4, gateway guard at CONFIG time: a remote_ts that covers the IKE
     * gateway would later route the tunnel's own outer packets into the
     * tunnel. Refuse to start -- deterministic and explainable, instead of
     * an encapsulation loop the user has to debug on a camera. (The same
     * check runs again against the NARROWED selectors before the route is
     * installed; this one catches the plain misconfiguration up front.) */
    if (d.cfg.have_remote_ts) {
        uint8_t gw[4];
        if (wd_resolve4(d.cfg.gateway, gw) == 0) {
            uint32_t g = ((uint32_t)gw[0] << 24) | ((uint32_t)gw[1] << 16)
                       | ((uint32_t)gw[2] << 8)  |  (uint32_t)gw[3];
            for (size_t i = 0; i < d.cfg.n_remote_ts; i++) {
                const wd_cidr *r = &d.cfg.remote_ts_list[i];
                uint32_t mask = (r->prefix == 0) ? 0 : 0xffffffffu << (32 - r->prefix);
                uint32_t n = ((uint32_t)r->ip[0] << 24) | ((uint32_t)r->ip[1] << 16)
                           | ((uint32_t)r->ip[2] << 8)  |  (uint32_t)r->ip[3];
                if ((g & mask) == (n & mask)) {
                    wd_log(LOG_ERR, "config: remote_subnet #%u contains the gateway "
                           "%u.%u.%u.%u -- refusing (encapsulation loop); exclude the "
                           "gateway from remote_subnet",
                           (unsigned)i, gw[0], gw[1], gw[2], gw[3]);
                    wd_config_wipe(&d.cfg);
                    return 1;
                }
            }
        }
    }

    /* ---- crypto ---- */
    if (weirdike_crypto_mbedtls_init(&d.mbed) != 0) {
        wd_log(LOG_ERR, "mbedTLS DRBG would not seed");
        wd_config_wipe(&d.cfg);
        return 1;
    }
    weirdike_crypto_mbedtls_bind(&d.mbed, &d.crypto);

    /* ---- sockets ---- */
    /* AP5: when the config pins an underlay, both sockets bind to that
     * concrete IP (and device). The transport then KNOWS its source without
     * the routing-table guess -- exactly what NAT-D needs, and a routing
     * flip can no longer move the tunnel. */
    {
        const uint8_t *bip = d.cfg.have_bind_ip ? d.cfg.bind_ip : NULL;
        const char    *bdv = d.cfg.bind_dev[0]  ? d.cfg.bind_dev : NULL;
        d.tr.fd500  = wd_udp_open(500, bip, bdv, err, sizeof(err));
        if (d.tr.fd500 < 0) { wd_log(LOG_ERR, "udp/500: %s", err); goto fail; }
        d.tr.fd4500 = wd_udp_open(4500, bip, bdv, err, sizeof(err));
        if (d.tr.fd4500 < 0) { wd_log(LOG_ERR, "udp/4500: %s", err); goto fail; }
        if (bip) { memcpy(d.tr.src_ip, bip, 4); d.tr.have_src = 1; }
    }
    d.tr.active_port = 500;

    /* ---- TUN ---- */
    d.tun_fd = wd_tun_open(d.cfg.ifname, err, sizeof(err));
    if (d.tun_fd < 0) { wd_log(LOG_ERR, "tun: %s", err); goto fail; }

    /* ---- the core ---- */
    d.vt.ctx = &d.tr;
    d.vt.open = tr_open; d.vt.resolve = tr_resolve; d.vt.local_endpoint = tr_local;
    d.vt.send = tr_send; d.vt.recv = tr_recv;       d.vt.close = tr_close;
    d.plat.log = core_log;

    weirdike_config_t wc;
    memset(&wc, 0, sizeof(wc));
    wc.server_host      = d.cfg.gateway;
    wc.server_port      = d.cfg.port;
    wc.auth             = WEIRDIKE_AUTH_PSK;
    wc.psk              = d.cfg.psk;
    wc.psk_len          = d.cfg.psk_len;
    wc.enable_nat_t     = d.cfg.nat_t;
    wc.child_lifetime_s = d.cfg.child_lifetime_s;
    wc.ike_lifetime_s   = d.cfg.ike_lifetime_s;
    wc.dpd_interval_s   = d.cfg.dpd_interval_s;

    if (d.cfg.have_local_ts)  ts_from_cidr(&wc.local_ts,  &d.cfg.local_ts);
    if (d.cfg.have_remote_ts) {
        /* AP6: first remote net is remote_ts, the rest go into
         * remote_ts_extra -- each its own TSr selector on the wire. */
        ts_from_cidr(&wc.remote_ts, &d.cfg.remote_ts_list[0]);
        size_t extra = d.cfg.n_remote_ts > 1 ? d.cfg.n_remote_ts - 1 : 0;
        if (extra > WEIRDIKE_TS_MAX - 1) extra = WEIRDIKE_TS_MAX - 1;
        for (size_t i = 0; i < extra; i++)
            ts_from_cidr(&wc.remote_ts_extra[i], &d.cfg.remote_ts_list[i + 1]);
        wc.n_remote_ts_extra = extra;
    } else {
        weirdike_ts_any_ipv4(&wc.remote_ts);
    }

    if (d.cfg.local_id[0]) {
        wc.local_id.type = WEIRDIKE_ID_FQDN;
        wc.local_id.data = (const uint8_t *)d.cfg.local_id;
        wc.local_id.len  = strlen(d.cfg.local_id);
    }
    if (d.cfg.remote_id[0]) {
        wc.remote_id.type = WEIRDIKE_ID_FQDN;
        wc.remote_id.data = (const uint8_t *)d.cfg.remote_id;
        wc.remote_id.len  = strlen(d.cfg.remote_id);
    }

    d.esp_scratch = malloc(ESP_MAX_PACKET);
    if (!d.esp_scratch) { wd_log(LOG_ERR, "out of memory for the ESP scratch"); goto fail; }

    d.ike = weirdike_new(&wc, &d.crypto, &d.vt, &d.plat, NULL);
    /* The config deep-copies the PSK; ours can go now. */
    wd_config_wipe(&d.cfg);
    memset(&wc, 0, sizeof(wc));
    if (!d.ike) { wd_log(LOG_ERR, "weirdike_new refused the configuration"); goto fail; }

    d.ctl_fd = ctl_listen(err, sizeof(err));
    if (d.ctl_fd < 0) { wd_log(LOG_ERR, "%s", err); goto fail; }

    signal(SIGTERM, on_signal);
    signal(SIGINT,  on_signal);
    signal(SIGPIPE, SIG_IGN);

    d.started_ms = wd_now_ms();
    if (weirdike_start(d.ike, d.started_ms) != 0) {
        wd_log(LOG_ERR, "weirdike_start failed");
        goto fail;
    }

    /* ------------------------------------------------------------- main loop */
    int failed_logged = 0;
    while (!g_signal && !d.want_stop) {
        uint32_t now = wd_now_ms();
        uint32_t wait = weirdike_next_deadline_ms(d.ike, now);
        if (wait > 1000) wait = 1000;       /* also the heartbeat for the control socket */

        struct pollfd p[4];
        p[0].fd = d.tr.fd500;  p[0].events = POLLIN;
        p[1].fd = d.tr.fd4500; p[1].events = POLLIN;
        p[2].fd = d.tun_fd;    p[2].events = POLLIN;
        p[3].fd = d.ctl_fd;    p[3].events = POLLIN;
        for (int i = 0; i < 4; i++) p[i].revents = 0;

        int rc = poll(p, 4, (int)wait);
        if (rc < 0 && errno != EINTR) { wd_log(LOG_ERR, "poll: %s", strerror(errno)); break; }

        if (p[0].revents & POLLIN) {
            uint8_t dg[WD_MAX_DGRAM];
            struct sockaddr_in sa; socklen_t sl = sizeof(sa);
            ssize_t n = recvfrom(d.tr.fd500, dg, sizeof(dg), 0,
                                 (struct sockaddr *)&sa, &sl);
            if (n > 0) {
                weirdike_endpoint_t from;
                memset(&from, 0, sizeof(from));
                memcpy(from.ip, &sa.sin_addr.s_addr, 4);
                from.port = ntohs(sa.sin_port);
                weirdike_input_datagram(d.ike, dg, (size_t)n, &from);
            }
        }
        if (p[1].revents & POLLIN) {
            uint8_t dg[WD_MAX_DGRAM];
            struct sockaddr_in sa; socklen_t sl = sizeof(sa);
            ssize_t n = recvfrom(d.tr.fd4500, dg, sizeof(dg), 0,
                                 (struct sockaddr *)&sa, &sl);
            if (n > 0) {
                weirdike_endpoint_t from;
                memset(&from, 0, sizeof(from));
                memcpy(from.ip, &sa.sin_addr.s_addr, 4);
                from.port = ntohs(sa.sin_port);
                on_udp4500(&d, dg, (size_t)n, &from);
            }
        }
        if (p[2].revents & POLLIN) pump_tun(&d);
        if (p[3].revents & POLLIN) ctl_serve(&d);

        weirdike_poll(d.ike, wd_now_ms());

        weirdike_state_t st = weirdike_state(d.ike);
        if (st == WEIRDIKE_STATE_CHILD_ESTABLISHED) esp_refresh(&d);
        if (weirdike_child_deleted(d.ike) && d.esp_up) {
            esp_session_deinit(&d.esp);
            d.esp_up = 0;
            wd_log(LOG_NOTICE, "child SA gone, data path down");
        }
        /* AP7 §3: a peer-initiated close (IKE SA DELETE -> CLOSED) also means
         * the tunnel is gone -- fail closed, same as a DPD failure. Without
         * this the routes/ipsec0 would linger after the peer hung up until the
         * daemon exits. Not on OUR OWN teardown (want_stop): the exit path
         * handles that. */
        if (st == WEIRDIKE_STATE_CLOSED && !d.want_stop &&
            (d.tun_configured || d.routes.n_owned || d.esp_up)) {
            wd_log(LOG_WARNING, "peer closed the tunnel -- tearing down data path");
            data_path_teardown(&d);
        }
        if (st == WEIRDIKE_STATE_FAILED && !failed_logged) {
            /* Log once, but KEEP RUNNING: the control socket must still be
             * able to answer "state=FAILED last_notify=..." -- both the AP2
             * negative interop cases and the AP3 status API read the failure
             * from here. Exiting made weirdikectl come back empty (roter
             * Lauf 2). The daemon ends on signal or ctl "down" only. */
            failed_logged = 1;
            weirdike_diag_t dg;
            if (weirdike_get_diag(d.ike, &dg) == 0) {
                wd_log(LOG_ERR, "negotiation failed: reached=%d notify=%u auth_rejected=%d local_auth_fail=%d",
                       dg.reached_state, (unsigned)dg.last_notify,
                       dg.ike_auth_rejected, dg.auth_local_fail);
            } else {
                wd_log(LOG_ERR, "negotiation failed");
            }
            /* AP7 §3: a FAILED that arrives AFTER the data path was up is a
             * lost tunnel (DPD gave up mid-session). Fail closed -- withdraw
             * routes and bring ipsec0 down so nothing rides the dead SA. The
             * daemon stays alive to report FAILED; machinod's reconnect policy
             * decides what happens next. */
            if (d.tun_configured || d.routes.n_owned || d.esp_up) {
                wd_log(LOG_WARNING, "tunnel lost after establishment -- tearing down data path");
                data_path_teardown(&d);
            }
        }
        /* A FAILED that later clears (a fresh connect attempt inside the same
         * process is not how this daemon works, but a peer that recovers the
         * IKE SA can) resets the one-shot latch. */
        if (st != WEIRDIKE_STATE_FAILED) failed_logged = 0;
    }

    if (g_signal) wd_log(LOG_NOTICE, "signal %d, shutting down", (int)g_signal);
    if (d.ike) weirdike_disconnect(d.ike, wd_now_ms());

fail:
    /* AP4/AP7: leave no residue -- routes withdrawn, ipsec0 down, ESP keys
     * zeroized. data_path_teardown is idempotent (safe even if FAILED already
     * tore it down, or if we bailed before the data path came up). */
    data_path_teardown(&d);
    if (d.ike)    weirdike_free(d.ike);
    free(d.esp_scratch);
    weirdike_crypto_mbedtls_free(&d.mbed);
    if (d.ctl_fd >= 0) { close(d.ctl_fd); unlink(WD_CTL_PATH); }
    if (d.tun_fd >= 0) close(d.tun_fd);
    if (d.tr.fd500  > 0) close(d.tr.fd500);
    if (d.tr.fd4500 > 0) close(d.tr.fd4500);
    wd_config_wipe(&d.cfg);
    if (!g_foreground) closelog();
    return 0;
}
