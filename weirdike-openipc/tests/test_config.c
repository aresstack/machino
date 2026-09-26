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

int main(void)
{
    t_minimal();
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
