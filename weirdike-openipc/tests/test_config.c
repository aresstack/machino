/*
 * weirdiked -- host tests for config parsing and input validation (AP34.13).
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * These are the checks that decide whether hostile or merely wrong input can
 * reach anything that matters. They run on the host with no camera, no
 * network, and no crypto.
 */
#include "wd_config.h"

#include <stdio.h>
#include <string.h>

static int fails = 0;
static int checks = 0;

#define CHECK(cond, what) do {                                        \
    checks++;                                                         \
    if (!(cond)) { printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, what); fails++; } \
} while (0)

static int parse(const char *text, wd_config *c, char *err, size_t errcap)
{
    return wd_config_parse(text, strlen(text), c, err, errcap);
}

static void t_minimal(void)
{
    wd_config c; char err[256] = {0};
    CHECK(parse("gateway = vpn.example.com\npsk = hunter2\n", &c, err, sizeof(err)) == 0, "minimal config parses");
    CHECK(!strcmp(c.gateway, "vpn.example.com"), "gateway kept");
    CHECK(c.port == 500, "default port 500");
    CHECK(c.nat_t == 1, "nat_t defaults on");
    CHECK(c.mtu == 1400, "default mtu");
    CHECK(!strcmp(c.ifname, "ipsec0"), "default interface");
    CHECK(c.psk_len == 7, "psk length");
    wd_config_wipe(&c);
    CHECK(c.psk_len == 0, "wipe clears the psk length");
    CHECK(c.psk[0] == 0, "wipe clears the psk bytes");
}

static void t_required(void)
{
    wd_config c; char err[256];
    CHECK(parse("psk = x\n", &c, err, sizeof(err)) != 0, "gateway is required");
    CHECK(strstr(err, "gateway") != NULL, "error names the missing key");
    CHECK(parse("gateway = a.b\n", &c, err, sizeof(err)) != 0, "psk is required");
}

static void t_unknown_key_is_an_error(void)
{
    wd_config c; char err[256];
    /* A typo must not leave a security-relevant setting at its default. */
    CHECK(parse("gateway = a.b\npsk = x\nnatt = 0\n", &c, err, sizeof(err)) != 0, "unknown key refused");
    CHECK(strstr(err, "natt") != NULL, "error names the unknown key");
}

static void t_hostname_validation(void)
{
    CHECK(wd_valid_hostname("vpn.example.com"), "fqdn ok");
    CHECK(wd_valid_hostname("192.0.2.1"), "ipv4 literal ok");
    CHECK(wd_valid_hostname("a-b.c"), "hyphen ok");
    CHECK(!wd_valid_hostname(""), "empty rejected");
    CHECK(!wd_valid_hostname("a b"), "space rejected");
    CHECK(!wd_valid_hostname("a;rm -rf /"), "semicolon rejected");
    CHECK(!wd_valid_hostname("$(id)"), "command substitution rejected");
    CHECK(!wd_valid_hostname("`id`"), "backtick rejected");
    CHECK(!wd_valid_hostname("a|b"), "pipe rejected");
    CHECK(!wd_valid_hostname("a\nb"), "newline rejected");
    CHECK(!wd_valid_hostname("-lead"), "leading hyphen rejected");
    CHECK(!wd_valid_hostname(".lead"), "leading dot rejected");
    CHECK(!wd_valid_hostname("trail."), "trailing dot rejected");
}

static void t_ifname_validation(void)
{
    CHECK(wd_valid_ifname("ipsec0"), "normal name ok");
    CHECK(!wd_valid_ifname(""), "empty rejected");
    CHECK(!wd_valid_ifname("../../etc/passwd"), "traversal rejected");
    CHECK(!wd_valid_ifname("eth0 up"), "space rejected");
    CHECK(!wd_valid_ifname("0123456789abcdefg"), "over-long rejected");
}

static void t_cidr(void)
{
    wd_cidr c;
    CHECK(wd_parse_cidr("10.0.0.0/8", &c) == 0 && c.ip[0] == 10 && c.prefix == 8, "cidr with prefix");
    CHECK(wd_parse_cidr("192.0.2.1", &c) == 0 && c.prefix == 32, "bare address is /32");
    CHECK(wd_parse_cidr("0.0.0.0/0", &c) == 0 && c.prefix == 0, "default route");
    CHECK(wd_parse_cidr("255.255.255.255/32", &c) == 0, "broadcast");

    CHECK(wd_parse_cidr("10.0.0.256", &c) != 0, "octet > 255 rejected");
    CHECK(wd_parse_cidr("10.0.0.1/33", &c) != 0, "prefix > 32 rejected");
    CHECK(wd_parse_cidr("10.0.0", &c) != 0, "three octets rejected");
    CHECK(wd_parse_cidr("10.0.0.1.2", &c) != 0, "five octets rejected");
    CHECK(wd_parse_cidr("10.0.0.010", &c) != 0, "leading zero rejected");
    CHECK(wd_parse_cidr("10.0.0.1/", &c) != 0, "empty prefix rejected");
    CHECK(wd_parse_cidr("10.0.0.1 ", &c) != 0, "trailing space rejected");
    CHECK(wd_parse_cidr("", &c) != 0, "empty rejected");
    CHECK(wd_parse_cidr("a.b.c.d", &c) != 0, "letters rejected");
}

static void t_ranges(void)
{
    wd_config c; char err[256];
    CHECK(parse("gateway=a.b\npsk=x\nport = 0\n", &c, err, sizeof(err)) != 0, "port 0 rejected");
    CHECK(parse("gateway=a.b\npsk=x\nport = 65536\n", &c, err, sizeof(err)) != 0, "port 65536 rejected");
    CHECK(parse("gateway=a.b\npsk=x\nport = 4500\n", &c, err, sizeof(err)) == 0 && c.port == 4500, "port 4500 ok");
    CHECK(parse("gateway=a.b\npsk=x\nmtu = 100\n", &c, err, sizeof(err)) != 0, "tiny mtu rejected");
    CHECK(parse("gateway=a.b\npsk=x\nnat_t = maybe\n", &c, err, sizeof(err)) != 0, "non-boolean rejected");
    CHECK(parse("gateway=a.b\npsk=x\nnat_t = no\n", &c, err, sizeof(err)) == 0 && c.nat_t == 0, "nat_t=no");
}

static void t_comments_and_whitespace(void)
{
    wd_config c; char err[256];
    const char *t =
        "# a comment\n"
        "\n"
        "   gateway   =   vpn.example.com   \n"
        "\tpsk\t=\tsecret\t\n"
        "# trailing comment\n";
    CHECK(parse(t, &c, err, sizeof(err)) == 0, "comments and whitespace");
    CHECK(!strcmp(c.gateway, "vpn.example.com"), "trimmed value");
    CHECK(c.psk_len == 6, "tab-separated psk");
}

static void t_error_never_leaks_the_psk(void)
{
    wd_config c; char err[256] = {0};
    /* A later key is bad; the error must not carry the secret that came before. */
    CHECK(parse("gateway=a.b\npsk=SUPERSECRET\nbogus=1\n", &c, err, sizeof(err)) != 0, "bad key fails");
    CHECK(strstr(err, "SUPERSECRET") == NULL, "error message holds no psk");
}

static void t_long_line(void)
{
    wd_config c; char err[256];
    char big[700];
    memset(big, 'a', sizeof(big) - 1);
    big[sizeof(big) - 1] = 0;
    char text[900];
    snprintf(text, sizeof(text), "gateway = %s\n", big);
    CHECK(parse(text, &c, err, sizeof(err)) != 0, "over-long line refused, not truncated");
}

static void t_bind_underlay(void)
{
    /* AP5: bind_ip pins the socket source, bind_dev the device. Literals
     * only -- a NAME here could resolve over the wrong interface. */
    wd_config c; char err[256];
    CHECK(parse("gateway=a.b\npsk=x\nbind_ip = 10.98.0.2\nbind_dev = usb0\n",
                &c, err, sizeof(err)) == 0, "bind keys accepted");
    CHECK(c.have_bind_ip && c.bind_ip[0] == 10 && c.bind_ip[3] == 2, "bind_ip parsed");
    CHECK(strcmp(c.bind_dev, "usb0") == 0, "bind_dev parsed");

    CHECK(parse("gateway=a.b\npsk=x\n", &c, err, sizeof(err)) == 0, "bind keys optional");
    CHECK(!c.have_bind_ip && !c.bind_dev[0], "unset = bind any (pre-AP5 behaviour)");

    CHECK(parse("gateway=a.b\npsk=x\nbind_ip = vpn.example.org\n",
                &c, err, sizeof(err)) != 0, "bind_ip name rejected");
    CHECK(parse("gateway=a.b\npsk=x\nbind_dev = not/valid\n",
                &c, err, sizeof(err)) != 0, "bad bind_dev rejected");
}

static void t_multi_remote_subnet(void)
{
    /* AP6: remote_subnet is a comma-separated list; remote_ts mirrors [0]. */
    wd_config c; char err[256];
    CHECK(parse("gateway=a.b\npsk=x\nremote_subnet = 192.168.178.0/24, 10.20.0.0/16\n",
                &c, err, sizeof(err)) == 0, "two remote subnets accepted");
    CHECK(c.n_remote_ts == 2, "two parsed");
    CHECK(c.remote_ts_list[0].ip[0] == 192 && c.remote_ts_list[0].prefix == 24, "first is 192.168.178/24");
    CHECK(c.remote_ts_list[1].ip[0] == 10 && c.remote_ts_list[1].prefix == 16, "second is 10.20/16");
    CHECK(c.remote_ts.ip[0] == 192, "remote_ts mirrors [0]");

    CHECK(parse("gateway=a.b\npsk=x\nremote_subnet = 1.0.0.0/8,2.0.0.0/8,3.0.0.0/8,4.0.0.0/8,5.0.0.0/8\n",
                &c, err, sizeof(err)) != 0, "more than 4 refused");
    CHECK(parse("gateway=a.b\npsk=x\nremote_subnet = 10.0.0.0/8, garbage\n",
                &c, err, sizeof(err)) != 0, "one bad entry fails the whole list");
    CHECK(parse("gateway=a.b\npsk=x\nremote_subnet = 10.0.0.0/8\n",
                &c, err, sizeof(err)) == 0 && c.n_remote_ts == 1, "single still works");
}

static void t_eap_mschapv2(void)
{
    /* AP9: auth=eap-mschapv2 braucht eap_user+eap_password statt psk. */
    wd_config c; char err[256];
    CHECK(parse("gateway=a.b\nauth=eap-mschapv2\neap_user=u@x\neap_password=pw\n"
                "trust_mode=host-store\n", &c, err, sizeof(err)) == 0, "eap config ok");
    CHECK(c.auth == 1, "auth=eap parsed");
    CHECK(strcmp(c.eap_user, "u@x") == 0, "eap_user parsed");
    CHECK(c.eap_password_len == 2, "eap_password parsed");
    CHECK(c.trust_mode == 1, "trust_mode=host-store");

    /* eap ohne Passwort -> Fehler (nicht psk-Fehler). */
    CHECK(parse("gateway=a.b\nauth=eap-mschapv2\neap_user=u@x\n",
                &c, err, sizeof(err)) != 0, "eap without password refused");
    /* eap braucht KEIN psk. */
    CHECK(parse("gateway=a.b\nauth=eap-mschapv2\neap_user=u@x\neap_password=pw\n",
                &c, err, sizeof(err)) == 0, "eap needs no psk");
    /* psk-Modus braucht weiterhin psk. */
    CHECK(parse("gateway=a.b\nauth=psk\n", &c, err, sizeof(err)) != 0, "psk mode still needs psk");
    /* bad trust_mode benannt abgelehnt. */
    CHECK(parse("gateway=a.b\nauth=eap-mschapv2\neap_user=u\neap_password=p\ntrust_mode=xxx\n",
                &c, err, sizeof(err)) != 0, "bad trust_mode refused");
    /* wipe loescht auch das EAP-Passwort. */
    CHECK(parse("gateway=a.b\nauth=eap-mschapv2\neap_user=u\neap_password=secretpw\n",
                &c, err, sizeof(err)) == 0, "eap parse for wipe");
    wd_config_wipe(&c);
    CHECK(c.eap_password_len == 0, "wipe cleared eap_password_len");
}

static void t_profile_fields(void)
{
    /* AP10 (WeirdOS profile parity): explicit identity types, a Configuration
     * Payload request with PSK, the PFS group. All optional; absent = the
     * pre-AP10 behaviour, so an existing weirdike.conf keeps meaning the same. */
    wd_config c; char err[256];
    CHECK(parse("gateway=a.b\npsk=x\nlocal_id=cam@intern\nlocal_id_type=rfc822\n"
                "remote_id=203.0.113.5\nremote_id_type=ipv4\nrequest_cp=1\npfs_group=14\n",
                &c, err, sizeof(err)) == 0, "profile keys accepted");
    CHECK(c.local_id_type == WD_ID_RFC822 && c.remote_id_type == WD_ID_IPV4, "id types parsed");
    CHECK(c.request_cp == 1 && c.pfs_group == 14, "request_cp + pfs_group parsed");

    CHECK(parse("gateway=a.b\npsk=x\nlocal_id=cam\nlocal_id_type=keyid\n", &c, err, sizeof(err)) == 0, "keyid accepted");
    CHECK(c.local_id_type == WD_ID_KEYID, "keyid parsed");

    CHECK(parse("gateway=a.b\npsk=x\n", &c, err, sizeof(err)) == 0, "profile keys optional");
    CHECK(c.local_id_type == WD_ID_FQDN && c.remote_id_type == WD_ID_FQDN, "default id type = fqdn (pre-AP10)");
    CHECK(c.request_cp == 0 && c.pfs_group == 0, "default: no CP request, no PFS (pre-AP10)");

    CHECK(parse("gateway=a.b\npsk=x\nlocal_id_type=email\n", &c, err, sizeof(err)) != 0, "bad id type refused");
    CHECK(strstr(err, "local_id_type") != NULL, "bad id type names the key");
    CHECK(parse("gateway=a.b\npsk=x\nlocal_id=cam.example\nlocal_id_type=ipv4\n", &c, err, sizeof(err)) != 0, "ipv4 type needs a literal");
    CHECK(parse("gateway=a.b\npsk=x\nremote_id=1.2.3\nremote_id_type=ipv4\n", &c, err, sizeof(err)) != 0, "three octets are not an address");
    CHECK(parse("gateway=a.b\npsk=x\npfs_group=99\n", &c, err, sizeof(err)) != 0, "pfs_group out of range");
    CHECK(parse("gateway=a.b\npsk=x\nrequest_cp=maybe\n", &c, err, sizeof(err)) != 0, "bad request_cp refused");

    uint8_t ip[4];
    CHECK(wd_parse_ipv4("10.0.0.1", ip) == 0 && ip[0] == 10 && ip[3] == 1, "ipv4 literal");
    CHECK(wd_parse_ipv4("10.0.0.256", ip) != 0, "octet range");
    CHECK(wd_parse_ipv4(" 10.0.0.1", ip) != 0, "no leading space");
    CHECK(wd_parse_ipv4("10.0.0.1/32", ip) != 0, "no prefix on a plain address");
}

static void t_policy_lists(void)
{
    /* AP11: allow-lists as catalogue names -> IANA ids; all five or none;
     * unknown names refused by name; liveness knobs. */
    wd_config c; char err[256];
    CHECK(parse("gateway=a.b\npsk=x\nike_dh = dh14, dh19\nike_enc=aes256cbc,aes128cbc\nike_hash=sha256,sha1\n"
                "esp_enc=aes256cbc\nesp_hash=sha256\n", &c, err, sizeof(err)) == 0, "policy lists parse");
    CHECK(c.have_policy, "have_policy");
    CHECK(c.ike_dh.n == 2 && c.ike_dh.id[0] == 14 && c.ike_dh.id[1] == 19, "dh ids");
    CHECK(c.ike_enc.n == 2 && c.ike_enc.id[0] == 12 && c.ike_enc.bits[0] == 256 && c.ike_enc.bits[1] == 128, "enc ids + bits");
    CHECK(c.ike_hash.n == 2 && c.ike_hash.id[0] == 5 && c.ike_hash.id2[0] == 12 && c.ike_hash.id[1] == 2 && c.ike_hash.id2[1] == 2, "ike hash prf+integ");
    CHECK(c.esp_hash.n == 1 && c.esp_hash.id[0] == 12, "esp integ");

    CHECK(parse("gateway=a.b\npsk=x\n", &c, err, sizeof(err)) == 0 && !c.have_policy, "no lists = engine default");
    CHECK(parse("gateway=a.b\npsk=x\nike_dh=dh14\n", &c, err, sizeof(err)) != 0, "partial policy refused");
    CHECK(strstr(err, "all five") != NULL, "partial policy names the rule");
    CHECK(parse("gateway=a.b\npsk=x\nike_dh=dh14\nike_enc=aes256cbc\nike_hash=sha256\nesp_enc=aes256cbc\nesp_hash=blake2\n",
                &c, err, sizeof(err)) != 0, "unknown algorithm refused");
    CHECK(strstr(err, "blake2") != NULL, "unknown algorithm named");
    CHECK(parse("gateway=a.b\npsk=x\nike_dh=dh14\nike_enc=aes256cbc\nike_hash=sha256\nesp_enc=aes256cbc\nesp_hash= , \n",
                &c, err, sizeof(err)) != 0, "empty list refused");
    /* The LANCOM grid is KNOWN in full; whether the build can run it is the engine's call. */
    CHECK(parse("gateway=a.b\npsk=x\nike_dh=dh2\nike_enc=chacha20\nike_hash=md5\nesp_enc=null\nesp_hash=null\n",
                &c, err, sizeof(err)) == 0, "unimplemented names still parse (engine refuses at start)");
    CHECK(c.ike_enc.id[0] == 28 && c.ike_hash.id[0] == 1 && c.esp_enc.id[0] == 11, "grid ids");

    uint16_t id, id2, bits;
    CHECK(wd_algo_lookup(2, "sha512", &id, &id2, &bits) == 0 && id == 7 && id2 == 14, "lookup ike hash");
    CHECK(wd_algo_lookup(0, "dh99", &id, &id2, &bits) != 0, "lookup unknown");
    CHECK(strcmp(wd_algo_name(1, 12, 256), "aes256cbc") == 0, "name enc");
    CHECK(strcmp(wd_algo_name(4, 12, 0), "sha256") == 0, "name esp integ");
    CHECK(strcmp(wd_algo_name(0, 99, 0), "?") == 0, "name unknown");

    CHECK(parse("gateway=a.b\npsk=x\ndpd=no\ndpd_retries=8\nnatt_keepalive_s=15\nchild_lifetime_kb=1048576\n",
                &c, err, sizeof(err)) == 0, "liveness keys");
    CHECK(c.dpd_disable == 1 && c.dpd_retries == 8 && c.natt_keepalive_s == 15 && c.child_lifetime_kb == 1048576, "liveness parsed");
    CHECK(parse("gateway=a.b\npsk=x\n", &c, err, sizeof(err)) == 0 && !c.dpd_disable && !c.dpd_retries && !c.natt_keepalive_s && !c.child_lifetime_kb, "liveness defaults");
    CHECK(parse("gateway=a.b\npsk=x\ndpd_retries=21\n", &c, err, sizeof(err)) != 0, "dpd_retries range");
    CHECK(parse("gateway=a.b\npsk=x\nnatt_keepalive_s=3\n", &c, err, sizeof(err)) != 0, "keepalive minimum");
}

int main(void)
{
    t_minimal();
    t_profile_fields();
    t_policy_lists();
    t_bind_underlay();
    t_multi_remote_subnet();
    t_eap_mschapv2();
    t_required();
    t_unknown_key_is_an_error();
    t_hostname_validation();
    t_ifname_validation();
    t_cidr();
    t_ranges();
    t_comments_and_whitespace();
    t_error_never_leaks_the_psk();
    t_long_line();

    printf("%d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
