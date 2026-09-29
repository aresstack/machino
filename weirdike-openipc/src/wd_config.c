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
/* AP11: the catalogue. Same names as machino's ipsec_algos.cpp and WeirdOS'
 * ipsec_crypto_caps.cpp -- the daemon only translates, it does not judge:
 * every name of the LANCOM grid is KNOWN here, and weirdike_policy_check()
 * decides at start what this build can honour. */
typedef struct { const char *name; uint16_t id; uint16_t id2; uint16_t bits; } wd_algo_def;
static const wd_algo_def kDh[] = {
    {"dh2", 2, 0, 0}, {"dh5", 5, 0, 0}, {"dh14", 14, 0, 0}, {"dh15", 15, 0, 0}, {"dh16", 16, 0, 0},
    {"dh19", 19, 0, 0}, {"dh20", 20, 0, 0}, {"dh21", 21, 0, 0}, {"dh28", 28, 0, 0}, {"dh29", 29, 0, 0},
    {"dh30", 30, 0, 0}, {"dh31", 31, 0, 0}, {"dh32", 32, 0, 0},
};
static const wd_algo_def kEnc[] = {   /* IKE and ESP share the ENCR registry */
    {"aes128cbc", 12, 0, 128}, {"aes192cbc", 12, 0, 192}, {"aes256cbc", 12, 0, 256},
    {"aes128gcm", 20, 0, 128}, {"aes192gcm", 20, 0, 192}, {"aes256gcm", 20, 0, 256},
    {"3des", 3, 0, 0}, {"chacha20", 28, 0, 0}, {"null", 11, 0, 0},
};
static const wd_algo_def kIkeHash[] = {   /* id = PRF, id2 = INTEG */
    {"sha1", 2, 2, 0}, {"sha256", 5, 12, 0}, {"sha384", 6, 13, 0}, {"sha512", 7, 14, 0}, {"md5", 1, 1, 0},
};
static const wd_algo_def kEspHash[] = {   /* id = INTEG */
    {"sha1", 2, 0, 0}, {"sha256", 12, 0, 0}, {"sha384", 13, 0, 0}, {"sha512", 14, 0, 0}, {"md5", 1, 0, 0}, {"null", 0, 0, 0},
};
static const wd_algo_def *algo_table(int group, size_t *n)
{
    switch (group) {
    case 0: *n = sizeof(kDh) / sizeof(kDh[0]); return kDh;
    case 1: case 3: *n = sizeof(kEnc) / sizeof(kEnc[0]); return kEnc;
    case 2: *n = sizeof(kIkeHash) / sizeof(kIkeHash[0]); return kIkeHash;
    case 4: *n = sizeof(kEspHash) / sizeof(kEspHash[0]); return kEspHash;
    }
    *n = 0; return NULL;
}

int wd_algo_lookup(int group, const char *name, uint16_t *id, uint16_t *id2, uint16_t *bits)
{
    size_t n = 0; const wd_algo_def *t = algo_table(group, &n);
    for (size_t i = 0; i < n; i++) {
        if (!strcmp(t[i].name, name)) {
            if (id)   *id = t[i].id;
            if (id2)  *id2 = t[i].id2;
            if (bits) *bits = t[i].bits;
            return 0;
        }
    }
    return -1;
}

const char *wd_algo_name(int group, uint16_t id, uint16_t bits)
{
    size_t n = 0; const wd_algo_def *t = algo_table(group, &n);
    for (size_t i = 0; i < n; i++) if (t[i].id == id && t[i].bits == bits) return t[i].name;
    return "?";
}

int wd_parse_algo_list(int group, const char *csv, wd_algo_list *out, char *err, size_t errcap)
{
    memset(out, 0, sizeof(*out));
    const char *p = csv;
    while (p && *p) {
        while (*p == ' ' || *p == '\t' || *p == ',') p++;
        if (!*p) break;
        const char *e = p;
        while (*e && *e != ',') e++;
        const char *t = e;
        while (t > p && (t[-1] == ' ' || t[-1] == '\t')) t--;
        char name[24];
        size_t len = (size_t)(t - p);
        if (len == 0 || len >= sizeof(name)) { seterr(err, errcap, "bad algorithm name", NULL); return -1; }
        memcpy(name, p, len); name[len] = 0;
        uint16_t id, id2, bits;
        if (wd_algo_lookup(group, name, &id, &id2, &bits)) { seterr(err, errcap, "unknown algorithm: %s", name); return -1; }
        if (out->n >= WD_ALGO_MAX) { seterr(err, errcap, "too many algorithms in one list", NULL); return -1; }
        out->id[out->n] = id; out->id2[out->n] = id2; out->bits[out->n] = bits; out->n++;
        p = e;
    }
    if (out->n == 0) { seterr(err, errcap, "empty algorithm list", NULL); return -1; }
    return 0;
}

int wd_parse_ipv4(const char *s, uint8_t out[4])
{
    /* Strict: exactly four decimal octets, nothing before or after. */
    unsigned a, b, c, d; char extra;
    if (!s || sscanf(s, "%u.%u.%u.%u%c", &a, &b, &c, &d, &extra) != 4) return -1;
    if (a > 255 || b > 255 || c > 255 || d > 255) return -1;
    /* Refuse leading '+', spaces and the like that sscanf would swallow. */
    for (const char *p = s; *p; p++) if (!((*p >= '0' && *p <= '9') || *p == '.')) return -1;
    out[0] = (uint8_t)a; out[1] = (uint8_t)b; out[2] = (uint8_t)c; out[3] = (uint8_t)d;
    return 0;
}

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
        } else if (!strcmp(k, "local_id_type") || !strcmp(k, "remote_id_type")) {
            /* AP10: explicit identity type. No content sniffing: the operator
             * says what the gateway expects, the daemon sends exactly that. */
            int t;
            if      (!strcmp(v, "fqdn"))   t = WD_ID_FQDN;
            else if (!strcmp(v, "rfc822")) t = WD_ID_RFC822;
            else if (!strcmp(v, "ipv4"))   t = WD_ID_IPV4;
            else if (!strcmp(v, "keyid"))  t = WD_ID_KEYID;
            else { seterr(err, errcap, "bad %s (fqdn|rfc822|ipv4|keyid)", k); return -1; }
            if (k[0] == 'l') out->local_id_type = t; else out->remote_id_type = t;
        } else if (!strcmp(k, "ike_dh") || !strcmp(k, "ike_enc") || !strcmp(k, "ike_hash") ||
                   !strcmp(k, "esp_enc") || !strcmp(k, "esp_hash")) {
            /* AP11: the policy lists. */
            int group = !strcmp(k, "ike_dh") ? 0 : !strcmp(k, "ike_enc") ? 1 : !strcmp(k, "ike_hash") ? 2
                      : !strcmp(k, "esp_enc") ? 3 : 4;
            wd_algo_list *dst = group == 0 ? &out->ike_dh : group == 1 ? &out->ike_enc : group == 2 ? &out->ike_hash
                              : group == 3 ? &out->esp_enc : &out->esp_hash;
            char lerr[128];
            if (wd_parse_algo_list(group, v, dst, lerr, sizeof(lerr))) { seterr(err, errcap, "%s", lerr); return -1; }
        } else if (!strcmp(k, "dpd")) {
            int on;
            if (as_bool(v, &on)) { seterr(err, errcap, "bad dpd", NULL); return -1; }
            out->dpd_disable = !on;
        } else if (!strcmp(k, "dpd_retries")) {
            if (as_uint(v, 20, &out->dpd_retries)) { seterr(err, errcap, "bad dpd_retries (0..20)", NULL); return -1; }
        } else if (!strcmp(k, "natt_keepalive_s")) {
            if (as_uint(v, 600, &out->natt_keepalive_s) || (out->natt_keepalive_s && out->natt_keepalive_s < 5)) {
                seterr(err, errcap, "bad natt_keepalive_s (0 or 5..600)", NULL); return -1;
            }
        } else if (!strcmp(k, "child_lifetime_kb")) {
            if (as_uint(v, 0xFFFFFFFFu, &out->child_lifetime_kb)) { seterr(err, errcap, "bad child_lifetime_kb", NULL); return -1; }
        } else if (!strcmp(k, "request_cp")) {
            if (as_bool(v, &out->request_cp)) { seterr(err, errcap, "bad request_cp", NULL); return -1; }
        } else if (!strcmp(k, "pfs_group")) {
            if (as_uint(v, 32, &out->pfs_group)) { seterr(err, errcap, "bad pfs_group (0 = off, else the D-H group number)", NULL); return -1; }
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
    /* AP11: all five lists or none -- never a half policy. */
    {
        int given = (out->ike_dh.n > 0) + (out->ike_enc.n > 0) + (out->ike_hash.n > 0)
                  + (out->esp_enc.n > 0) + (out->esp_hash.n > 0);
        if (given == 5) out->have_policy = 1;
        else if (given) { seterr(err, errcap, "proposal policy needs all five lists (ike_dh, ike_enc, ike_hash, esp_enc, esp_hash)", NULL); return -1; }
    }
    /* AP10: an IPv4 identity is four bytes on the wire, so the value must be
     * a literal -- a name here would go out as garbage, not as an address. */
    {
        uint8_t ip[4];
        if (out->local_id[0] && out->local_id_type == WD_ID_IPV4 && wd_parse_ipv4(out->local_id, ip))
            { seterr(err, errcap, "local_id_type=ipv4 needs a dotted-quad local_id", NULL); return -1; }
        if (out->remote_id[0] && out->remote_id_type == WD_ID_IPV4 && wd_parse_ipv4(out->remote_id, ip))
            { seterr(err, errcap, "remote_id_type=ipv4 needs a dotted-quad remote_id", NULL); return -1; }
    }
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
