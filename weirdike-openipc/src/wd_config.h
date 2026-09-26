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
#define WD_MAX_REMOTE_TS 4    /* == WEIRDIKE_TS_MAX; AP6 multi-subnet request */

typedef struct {
    uint8_t  ip[4];
    uint8_t  prefix;      /* 0..32 */
} wd_cidr;

typedef struct {
    char     gateway[WD_MAX_HOST];    /* IPv4 literal or hostname */
    uint16_t port;                    /* default 500 */
    char     ifname[WD_MAX_IFNAME];   /* TUN interface, default ipsec0 */

    /* AP9: auth = 0 (PSK) | 1 (EAP-MSCHAPv2). Matches WEIRDIKE_AUTH_*. */
    int      auth;

    uint8_t  psk[WD_MAX_PSK];
    size_t   psk_len;

    /* AP9: EAP-MSCHAPv2 (auth==1). eap_user is the identity (username), the
     * password is a secret (zeroized on wipe). trust_mode matches
     * weirdike_trust_mode_t (0 ANCHOR_PEM,1 HOST_STORE,2 HOST_STORE_PLUS_PEM,
     * 3 NONE). ca_pem/extra_pem are PEM buffers read from 0600 files that
     * machinod owns; empty = none. */
    char     eap_user[WD_MAX_ID];
    uint8_t  eap_password[WD_MAX_PSK];
    size_t   eap_password_len;
    int      trust_mode;
    /* Paths the parser records (it stays a pure buffer function); load_config
     * reads the PEM files into the buffers below (the 8 KiB config text cannot
     * hold a 4 KiB cert inline). */
    char     ca_pem_file[128];
    char     extra_pem_file[128];
    char     ca_pem[4096];            /* WEIRDIKE_MAX_CA_PEM */
    size_t   ca_pem_len;
    char     extra_pem[4096];         /* WEIRDIKE_MAX_EXTRA_PEM */
    size_t   extra_pem_len;

    char     local_id[WD_MAX_ID];     /* empty = derive from source IP */
    char     remote_id[WD_MAX_ID];    /* empty = accept whatever the responder sends */

    wd_cidr  local_ts;                /* the network we protect */
    wd_cidr  remote_ts;               /* remote_ts_list[0]; kept for back-compat */
    int      have_local_ts;
    int      have_remote_ts;

    /* AP6: up to WD_MAX_REMOTE_TS remote nets, requested as a comma-separated
     * `remote_subnet`. remote_ts mirrors [0]. The responder may narrow or
     * drop entries; only the NEGOTIATED ones ever become routes. */
    wd_cidr  remote_ts_list[WD_MAX_REMOTE_TS];
    size_t   n_remote_ts;

    int      nat_t;                   /* default 1 */
    uint32_t child_lifetime_s;        /* 0 = library default */
    uint32_t ike_lifetime_s;          /* 0 = never self-initiated */
    uint32_t dpd_interval_s;          /* 0 = library default */
    int      mtu;                     /* TUN MTU, default 1400 */

    /* AP5: pin IKE/ESP to ONE underlay. bind_ip is the concrete local IPv4
     * the sockets bind to (machinod picks it from the selected uplink);
     * bind_dev additionally pins the device (SO_BINDTODEVICE), so a later
     * routing change cannot silently move the tunnel to another interface.
     * Both empty = bind to any (the pre-AP5 behaviour). The daemon has no
     * idea WHAT the underlay is -- no modem words in here. */
    uint8_t  bind_ip[4];
    int      have_bind_ip;
    char     bind_dev[WD_MAX_IFNAME];
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
