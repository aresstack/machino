/*
 * weirdiked -- configuration: parse, validate, and keep the secret out of everything else.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The parser is a pure function over a buffer so it can be tested on the host
 * without a filesystem. Everything an attacker could influence -- the gateway
 * string, the subnets, the interface name -- is validated here and nowhere
 * else, and none of it is ever passed to a shell.
 */
#ifndef WD_CONFIG_H
#define WD_CONFIG_H

#include <stdint.h>
#include <stddef.h>

#define WD_MAX_HOST   128
#define WD_MAX_PSK    128
#define WD_MAX_IFNAME  16
#define WD_MAX_ID      128

typedef struct {
    uint8_t  ip[4];
    uint8_t  prefix;      /* 0..32 */
} wd_cidr;

typedef struct {
    char     gateway[WD_MAX_HOST];    /* IPv4 literal or hostname */
    uint16_t port;                    /* default 500 */
    char     ifname[WD_MAX_IFNAME];   /* TUN interface, default ipsec0 */

    uint8_t  psk[WD_MAX_PSK];
    size_t   psk_len;

    char     local_id[WD_MAX_ID];     /* empty = derive from source IP */
    char     remote_id[WD_MAX_ID];    /* empty = accept whatever the responder sends */

    wd_cidr  local_ts;                /* the network we protect */
    wd_cidr  remote_ts;               /* the network behind the gateway */
    int      have_local_ts;
    int      have_remote_ts;

    int      nat_t;                   /* default 1 */
    uint32_t child_lifetime_s;        /* 0 = library default */
    uint32_t ike_lifetime_s;          /* 0 = never self-initiated */
    uint32_t dpd_interval_s;          /* 0 = library default */
    int      mtu;                     /* TUN MTU, default 1400 */
} wd_config;

/* Parse `key = value` text. Unknown keys are an error, not a shrug: a typo in a
 * VPN config must not silently leave a setting at its default.
 *
 * Returns 0 on success. On failure returns -1 and writes a one-line reason into
 * err (never containing the PSK).
 */
int wd_config_parse(const char *text, size_t len, wd_config *out, char *err, size_t errcap);

/* Validation that does not depend on parsing, exposed for tests. */
int wd_valid_hostname(const char *s);            /* 1 = ok */
int wd_valid_ifname(const char *s);              /* 1 = ok */
int wd_parse_cidr(const char *s, wd_cidr *out);  /* 0 = ok */

/* Format a CIDR back out, for logs and status. Never fails. */
void wd_cidr_str(const wd_cidr *c, char *buf, size_t cap);

/* Wipe the secret. Call before the struct goes out of scope. */
void wd_config_wipe(wd_config *c);

#endif /* WD_CONFIG_H */
