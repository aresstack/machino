// hostapd.conf and udhcpd.conf generation.
//
// hostapd.conf is key=value with no quoting and no escaping whatsoever, so the
// injection test below is not a formality: a newline in an SSID appends
// directives of the submitter's choosing, and "wpa=0" among them turns a
// secured access point into an open one.
#include "core/net/hostapd_conf.hpp"
#include <cstdio>
#include <string>

using namespace machino;
using namespace machino::net;

extern int g_fail_ext, g_pass_ext;
#define TCHECK(cond) do { if (cond) { ++g_pass_ext; } else { ++g_fail_ext; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

namespace {

bool has(const std::string& hay, const std::string& needle)
{
    return hay.find(needle) != std::string::npos;
}

WifiApConfig good()
{
    WifiApConfig c;
    c.ssid = "machino-cam";
    c.passphrase = "supersecret1";
    c.security = WifiSecurity::Wpa2;
    c.channel = 6;
    c.ipv4 = "192.168.4.1";
    c.dhcp_start = "192.168.4.20";
    c.dhcp_end = "192.168.4.100";
    return c;
}

void test_a_normal_wpa2_access_point()
{
    std::string out, err;
    TCHECK(hostapd_conf_from(good(), "wlan0", "nl80211", out, err));
    TCHECK(has(out, "interface=wlan0"));
    TCHECK(has(out, "driver=nl80211"));
    TCHECK(has(out, "ssid=machino-cam"));
    TCHECK(has(out, "hw_mode=g"));
    TCHECK(has(out, "channel=6"));
    TCHECK(has(out, "wpa=2"));
    TCHECK(has(out, "wpa_key_mgmt=WPA-PSK"));
    TCHECK(has(out, "rsn_pairwise=CCMP"));
    TCHECK(has(out, "wpa_passphrase=supersecret1"));
    // The control interface is the whole reason machino never has to start a
    // process to reconfigure the AP.
    TCHECK(has(out, "ctrl_interface="));
}

void test_an_ssid_cannot_inject_directives()
{
    WifiApConfig c = good();
    c.ssid = "evil\nwpa=0";
    std::string out, err;
    TCHECK(!hostapd_conf_from(c, "wlan0", "nl80211", out, err));
    TCHECK(out.empty());
    TCHECK(!err.empty());

    c.ssid = "evil\rmore";
    TCHECK(!hostapd_conf_from(c, "wlan0", "nl80211", out, err));
    c.ssid = std::string("evil\0hidden", 11);
    TCHECK(!hostapd_conf_from(c, "wlan0", "nl80211", out, err));
}

void test_a_passphrase_cannot_inject_directives()
{
    WifiApConfig c = good();
    c.passphrase = "12345678\nignore_broadcast_ssid=1";
    std::string out, err;
    TCHECK(!hostapd_conf_from(c, "wlan0", "nl80211", out, err));
    TCHECK(out.empty());
}

void test_the_passphrase_length_hostapd_will_accept()
{
    // hostapd refuses to START outside 8..63 rather than falling back, and a
    // camera whose access point never came up is the recovery path failing.
    WifiApConfig c = good();
    std::string out, err;

    c.passphrase = "short7c";
    TCHECK(!hostapd_conf_from(c, "wlan0", "nl80211", out, err));
    c.passphrase = std::string(64, 'x');
    TCHECK(!hostapd_conf_from(c, "wlan0", "nl80211", out, err));

    c.passphrase = std::string(8, 'x');
    TCHECK(hostapd_conf_from(c, "wlan0", "nl80211", out, err));
    c.passphrase = std::string(63, 'x');
    TCHECK(hostapd_conf_from(c, "wlan0", "nl80211", out, err));
}

void test_an_open_access_point_is_written_as_open()
{
    WifiApConfig c = good();
    c.security = WifiSecurity::Open;
    c.passphrase.clear();
    std::string out, err;
    TCHECK(hostapd_conf_from(c, "wlan0", "nl80211", out, err));
    TCHECK(has(out, "wpa=0"));
    TCHECK(!has(out, "wpa_passphrase"));
}

void test_wpa3_and_the_mixed_mode()
{
    WifiApConfig c = good();
    std::string out, err;

    c.security = WifiSecurity::Wpa3;
    TCHECK(hostapd_conf_from(c, "wlan0", "nl80211", out, err));
    TCHECK(has(out, "wpa_key_mgmt=SAE"));
    // SAE requires management frame protection; without it hostapd refuses.
    TCHECK(has(out, "ieee80211w=2"));

    c.security = WifiSecurity::Wpa2Wpa3;
    TCHECK(hostapd_conf_from(c, "wlan0", "nl80211", out, err));
    TCHECK(has(out, "wpa_key_mgmt=WPA-PSK SAE"));
    // Optional, not required: required would lock out every WPA2-only client,
    // which defeats the point of a mixed mode.
    TCHECK(has(out, "ieee80211w=1"));
}

void test_wep_and_wpa1_are_refused_for_an_ap_we_create()
{
    // Reading them off the air is one thing; offering to create one is another.
    WifiApConfig c = good();
    std::string out, err;
    c.security = WifiSecurity::Wep;
    TCHECK(!hostapd_conf_from(c, "wlan0", "nl80211", out, err));
    c.security = WifiSecurity::Wpa;
    TCHECK(!hostapd_conf_from(c, "wlan0", "nl80211", out, err));
}

void test_channels_and_bands()
{
    std::string mode;
    TCHECK(hostapd_band_for_channel(1, mode) && mode == "g");
    TCHECK(hostapd_band_for_channel(13, mode) && mode == "g");
    TCHECK(hostapd_band_for_channel(36, mode) && mode == "a");
    TCHECK(hostapd_band_for_channel(149, mode) && mode == "a");
    TCHECK(hostapd_band_for_channel(0, mode) && mode == "g");   // auto
    TCHECK(!hostapd_band_for_channel(20, mode));                // between the bands
    TCHECK(!hostapd_band_for_channel(-1, mode));
    TCHECK(!hostapd_band_for_channel(300, mode));

    WifiApConfig c = good();
    c.channel = 36;
    std::string out, err;
    TCHECK(hostapd_conf_from(c, "wlan0", "nl80211", out, err));
    TCHECK(has(out, "hw_mode=a"));
}

void test_an_ssid_longer_than_the_standard_allows()
{
    WifiApConfig c = good();
    c.ssid = std::string(33, 'x');
    std::string out, err;
    TCHECK(!hostapd_conf_from(c, "wlan0", "nl80211", out, err));
    c.ssid = std::string(32, 'x');
    TCHECK(hostapd_conf_from(c, "wlan0", "nl80211", out, err));
}

void test_the_dhcp_pool()
{
    std::string out, err;
    TCHECK(udhcpd_conf_from(good(), "wlan0", out, err));
    TCHECK(has(out, "interface wlan0"));
    TCHECK(has(out, "start 192.168.4.20"));
    TCHECK(has(out, "end 192.168.4.100"));
    TCHECK(has(out, "option router 192.168.4.1"));
    TCHECK(has(out, "option subnet 255.255.255.0"));
}

void test_the_router_must_not_be_inside_its_own_pool()
{
    // udhcpd will hand out the router's address and the whole segment stops
    // working, in a way that looks like a driver fault.
    WifiApConfig c = good();
    c.ipv4 = "192.168.4.50";
    std::string out, err;
    TCHECK(!udhcpd_conf_from(c, "wlan0", out, err));
    TCHECK(has(err, "pool"));
}

void test_a_backwards_or_off_subnet_pool_is_refused()
{
    WifiApConfig c = good();
    std::string out, err;

    c.dhcp_start = "192.168.4.100"; c.dhcp_end = "192.168.4.20";
    TCHECK(!udhcpd_conf_from(c, "wlan0", out, err));

    c = good();
    c.dhcp_end = "192.168.9.100";
    TCHECK(!udhcpd_conf_from(c, "wlan0", out, err));

    c = good();
    c.ipv4 = "not-an-address";
    TCHECK(!udhcpd_conf_from(c, "wlan0", out, err));
}

} // namespace

void run_hostapd_conf_tests()
{
    test_a_normal_wpa2_access_point();
    test_an_ssid_cannot_inject_directives();
    test_a_passphrase_cannot_inject_directives();
    test_the_passphrase_length_hostapd_will_accept();
    test_an_open_access_point_is_written_as_open();
    test_wpa3_and_the_mixed_mode();
    test_wep_and_wpa1_are_refused_for_an_ap_we_create();
    test_channels_and_bands();
    test_an_ssid_longer_than_the_standard_allows();
    test_the_dhcp_pool();
    test_the_router_must_not_be_inside_its_own_pool();
    test_a_backwards_or_off_subnet_pool_is_refused();
}
