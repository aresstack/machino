/*
 * weirdiked -- configuration parsing and validation.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "wd_config.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static void seterr(char *err, size_t cap, const char *fmt, const char *a)
{
    if (!err || !cap) return;
    if (a) snprintf(err, cap, fmt, a);
    else   snprintf(err, cap, "%s", fmt);
}

int wd_valid_hostname(const char *s)
{
    size_t n = s ? strlen(s) : 0;
    if (n == 0 || n >= WD_MAX_HOST) return 0;
    /* Deliberately strict: letters, digits, dot, hyphen. That covers every IPv4
     * literal and every hostname we care about, and it excludes the whole class
     * of characters a shell or a config file would treat specially. weirdiked
     * never invokes a shell, but a value that cannot be dangerous is better
     * than one that merely is not, today. */
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        int ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                 (c >= '0' && c <= '9') || c == '.' || c == '-';
        if (!ok) return 0;
    }
    if (s[0] == '-' || s[0] == '.' || s[n - 1] == '.') return 0;
    return 1;
}

int wd_valid_ifname(const char *s)
{
    size_t n = s ? strlen(s) : 0;
    if (n == 0 || n >= WD_MAX_IFNAME) return 0;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        int ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                 (c >= '0' && c <= '9') || c == '-' || c == '_';
        if (!ok) return 0;
    }
    /* '/' and ':' would confuse sysfs paths and ifconfig output; excluded above. */
    return 1;
}

/* "a.b.c.d" or "a.b.c.d/p". Rejects leading zeros, out-of-range octets, a
 * prefix > 32, and anything with trailing garbage. */
int wd_parse_cidr(const char *s, wd_cidr *out)
{
    if (!s || !out) return -1;
    unsigned v[4]; unsigned pfx = 32;
    size_t i = 0, n = strlen(s);
    if (n == 0 || n > 18) return -1;

    for (int o = 0; o < 4; o++) {
        unsigned acc = 0; size_t digits = 0;
        while (i < n && s[i] >= '0' && s[i] <= '9') {
            acc = acc * 10 + (unsigned)(s[i] - '0');
            if (++digits > 3 || acc > 255) return -1;
            i++;
        }
        if (digits == 0) return -1;
        if (digits > 1 && s[i - digits] == '0') return -1;   /* no 010 */
        v[o] = acc;
        if (o < 3) { if (i >= n || s[i] != '.') return -1; i++; }
    }
    if (i < n) {
        if (s[i] != '/') return -1;
        i++;
        unsigned acc = 0; size_t digits = 0;
        while (i < n && s[i] >= '0' && s[i] <= '9') {
            acc = acc * 10 + (unsigned)(s[i] - '0');
            if (++digits > 2) return -1;
            i++;
        }
        if (digits == 0 || acc > 32) return -1;
        if (digits > 1 && s[i - digits] == '0') return -1;
        pfx = acc;
    }
    if (i != n) return -1;

    out->ip[0] = (uint8_t)v[0]; out->ip[1] = (uint8_t)v[1];
    out->ip[2] = (uint8_t)v[2]; out->ip[3] = (uint8_t)v[3];
    out->prefix = (uint8_t)pfx;
    return 0;
}

void wd_cidr_str(const wd_cidr *c, char *buf, size_t cap)
{
    if (!buf || !cap) return;
    if (!c) { snprintf(buf, cap, "-"); return; }
    snprintf(buf, cap, "%u.%u.%u.%u/%u",
             c->ip[0], c->ip[1], c->ip[2], c->ip[3], c->prefix);
}

static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r')) *--e = 0;
    return s;
}

static int as_uint(const char *v, uint32_t max, uint32_t *out)
{
    if (!*v) return -1;
    char *end = NULL;
    unsigned long x = strtoul(v, &end, 10);
    if (!end || *end || x > max) return -1;
    *out = (uint32_t)x;
    return 0;
}

static int as_bool(const char *v, int *out)
{
    if (!strcmp(v, "1") || !strcmp(v, "yes") || !strcmp(v, "true"))  { *out = 1; return 0; }
    if (!strcmp(v, "0") || !strcmp(v, "no")  || !strcmp(v, "false")) { *out = 0; return 0; }
    return -1;
}

int wd_config_parse(const char *text, size_t len, wd_config *out, char *err, size_t errcap)
{
    if (!text || !out) { seterr(err, errcap, "no input", NULL); return -1; }

    memset(out, 0, sizeof(*out));
    out->port = 500;
    out->nat_t = 1;
    out->mtu = 1400;
    snprintf(out->ifname, sizeof(out->ifname), "%s", "ipsec0");

    char line[512];
    size_t pos = 0, lineno = 0;

    while (pos < len) {
        size_t e = pos;
        while (e < len && text[e] != '\n') e++;
        size_t n = e - pos;
        lineno++;
        if (n >= sizeof(line)) { seterr(err, errcap, "line too long", NULL); return -1; }
        memcpy(line, text + pos, n);
        line[n] = 0;
        pos = (e < len) ? e + 1 : len;

        char *s = trim(line);
        if (!*s || *s == '#') continue;

        char *eq = strchr(s, '=');
        if (!eq) { seterr(err, errcap, "not key = value: %s", s); return -1; }
        *eq = 0;
        char *k = trim(s);
        char *v = trim(eq + 1);

        if (!strcmp(k, "gateway")) {
            if (!wd_valid_hostname(v)) { seterr(err, errcap, "bad gateway", NULL); return -1; }
            snprintf(out->gateway, sizeof(out->gateway), "%s", v);
        } else if (!strcmp(k, "port")) {
            uint32_t x; if (as_uint(v, 65535, &x) || x == 0) { seterr(err, errcap, "bad port", NULL); return -1; }
            out->port = (uint16_t)x;
        } else if (!strcmp(k, "interface")) {
            if (!wd_valid_ifname(v)) { seterr(err, errcap, "bad interface name", NULL); return -1; }
            snprintf(out->ifname, sizeof(out->ifname), "%s", v);
        } else if (!strcmp(k, "psk")) {
            size_t pl = strlen(v);
            if (pl == 0 || pl > WD_MAX_PSK) { seterr(err, errcap, "psk missing or too long", NULL); return -1; }
            memcpy(out->psk, v, pl);
            out->psk_len = pl;
            /* scrub the copy that lives in our stack line buffer */
            memset(v, 0, pl);
        } else if (!strcmp(k, "auth")) {
            if (!strcmp(v, "psk")) out->auth = 0;
            else if (!strcmp(v, "eap-mschapv2")) out->auth = 1;
            else { seterr(err, errcap, "bad auth (psk|eap-mschapv2)", NULL); return -1; }
        } else if (!strcmp(k, "eap_user")) {
            if (strlen(v) >= WD_MAX_ID) { seterr(err, errcap, "eap_user too long", NULL); return -1; }
            snprintf(out->eap_user, sizeof(out->eap_user), "%s", v);
        } else if (!strcmp(k, "eap_password")) {
            size_t pl = strlen(v);
            if (pl == 0 || pl > WD_MAX_PSK) { seterr(err, errcap, "eap_password missing or too long", NULL); return -1; }
            memcpy(out->eap_password, v, pl);
            out->eap_password_len = pl;
            memset(v, 0, pl);          /* scrub the stack copy */
        } else if (!strcmp(k, "trust_mode")) {
            if (!strcmp(v, "anchor-pem")) out->trust_mode = 0;
            else if (!strcmp(v, "host-store")) out->trust_mode = 1;
            else if (!strcmp(v, "host-store-plus-pem")) out->trust_mode = 2;
            else if (!strcmp(v, "none")) out->trust_mode = 3;
            else { seterr(err, errcap, "bad trust_mode", NULL); return -1; }
        } else if (!strcmp(k, "ca_pem_file")) {
            if (strlen(v) >= sizeof(out->ca_pem_file)) { seterr(err, errcap, "ca_pem_file path too long", NULL); return -1; }
            snprintf(out->ca_pem_file, sizeof(out->ca_pem_file), "%s", v);
        } else if (!strcmp(k, "extra_pem_file")) {
            if (strlen(v) >= sizeof(out->extra_pem_file)) { seterr(err, errcap, "extra_pem_file path too long", NULL); return -1; }
            snprintf(out->extra_pem_file, sizeof(out->extra_pem_file), "%s", v);
        } else if (!strcmp(k, "local_id")) {
            if (strlen(v) >= WD_MAX_ID) { seterr(err, errcap, "local_id too long", NULL); return -1; }
            snprintf(out->local_id, sizeof(out->local_id), "%s", v);
        } else if (!strcmp(k, "remote_id")) {
            if (strlen(v) >= WD_MAX_ID) { seterr(err, errcap, "remote_id too long", NULL); return -1; }
            snprintf(out->remote_id, sizeof(out->remote_id), "%s", v);
        } else if (!strcmp(k, "local_subnet")) {
            if (wd_parse_cidr(v, &out->local_ts)) { seterr(err, errcap, "bad local_subnet", NULL); return -1; }
            out->have_local_ts = 1;
        } else if (!strcmp(k, "remote_subnet")) {
            /* AP6: comma-separated list, up to WD_MAX_REMOTE_TS. Each entry
             * becomes its own TSr selector; remote_ts mirrors the first. */
            /* Manual comma split -- no strtok_r, so `make test` needs no
             * feature-test macro (Linux gcc without _GNU_SOURCE hid it as an
             * implicit declaration; roter Lauf bf6a962). */
            out->n_remote_ts = 0;
            char *p = v;
            while (*p) {
                char *comma = strchr(p, ',');
                if (comma) *comma = 0;
                char *e = p; while (*e == ' ' || *e == '\t') e++;
                char *end = e + strlen(e);
                while (end > e && (end[-1] == ' ' || end[-1] == '\t')) *--end = 0;
                if (*e) {
                    if (out->n_remote_ts >= WD_MAX_REMOTE_TS) {
                        seterr(err, errcap, "too many remote_subnet entries (max 4)", NULL); return -1;
                    }
                    if (wd_parse_cidr(e, &out->remote_ts_list[out->n_remote_ts])) {
                        seterr(err, errcap, "bad remote_subnet", NULL); return -1;
                    }
                    out->n_remote_ts++;
                }
                if (!comma) break;
                p = comma + 1;
            }
            if (out->n_remote_ts == 0) { seterr(err, errcap, "empty remote_subnet", NULL); return -1; }
            out->remote_ts = out->remote_ts_list[0];
            out->have_remote_ts = 1;
        } else if (!strcmp(k, "nat_t")) {
            if (as_bool(v, &out->nat_t)) { seterr(err, errcap, "bad nat_t", NULL); return -1; }
        } else if (!strcmp(k, "child_lifetime_s")) {
            if (as_uint(v, 86400 * 7, &out->child_lifetime_s)) { seterr(err, errcap, "bad child_lifetime_s", NULL); return -1; }
        } else if (!strcmp(k, "ike_lifetime_s")) {
            if (as_uint(v, 86400 * 7, &out->ike_lifetime_s)) { seterr(err, errcap, "bad ike_lifetime_s", NULL); return -1; }
        } else if (!strcmp(k, "dpd_interval_s")) {
            if (as_uint(v, 3600, &out->dpd_interval_s)) { seterr(err, errcap, "bad dpd_interval_s", NULL); return -1; }
        } else if (!strcmp(k, "mtu")) {
            uint32_t x; if (as_uint(v, 9000, &x) || x < 576) { seterr(err, errcap, "bad mtu", NULL); return -1; }
            out->mtu = (int)x;
        } else if (!strcmp(k, "bind_ip")) {
            /* AP5: the concrete underlay IPv4 (a literal, never a name --
             * resolving here could pick a DIFFERENT interface's answer). */
            wd_cidr c;
            if (wd_parse_cidr(v, &c) || c.prefix != 32) {
                /* accept plain a.b.c.d too */
                char withp[24];
                snprintf(withp, sizeof(withp), "%s/32", v);
                if (wd_parse_cidr(withp, &c)) { seterr(err, errcap, "bad bind_ip", NULL); return -1; }
            }
            memcpy(out->bind_ip, c.ip, 4);
            out->have_bind_ip = 1;
        } else if (!strcmp(k, "bind_dev")) {
            if (!wd_valid_ifname(v)) { seterr(err, errcap, "bad bind_dev", NULL); return -1; }
            snprintf(out->bind_dev, sizeof(out->bind_dev), "%s", v);
        } else {
            seterr(err, errcap, "unknown key: %s", k);
            return -1;
        }
    }

    if (!out->gateway[0]) { seterr(err, errcap, "gateway is required", NULL); return -1; }
    /* AP9: the required credential depends on the auth mode. */
    if (out->auth == 1) {
        if (!out->eap_user[0])     { seterr(err, errcap, "eap_user is required for eap-mschapv2", NULL); return -1; }
        if (!out->eap_password_len){ seterr(err, errcap, "eap_password is required for eap-mschapv2", NULL); return -1; }
    } else {
        if (!out->psk_len)         { seterr(err, errcap, "psk is required", NULL); return -1; }
    }

    (void)lineno;
    return 0;
}

void wd_config_wipe(wd_config *c)
{
    if (!c) return;
    memset(c->psk, 0, sizeof(c->psk));
    c->psk_len = 0;
    /* AP9: the EAP password is a secret too. (ca/extra PEM are public, no wipe
     * needed, but zeroing them is cheap and keeps the struct tidy.) */
    memset(c->eap_password, 0, sizeof(c->eap_password));
    c->eap_password_len = 0;
}
