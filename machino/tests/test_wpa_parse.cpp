// wpa_supplicant reply parsing, against output shaped like the real thing.
//
// The awkward cases are the point: an SSID with a space, a hidden network with
// no SSID at all, a truncated line, and a flag set we do not recognise.
#include "core/net/wpa_parse.hpp"
#include "ports/inetwork.hpp"
#include <cstdio>

using namespace machino;
using namespace machino::net;

extern int g_fail_ext, g_pass_ext;
#define TCHECK(c) do { if (c) { ++g_pass_ext; } else { ++g_fail_ext; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

namespace {

const char* kScan =
    "bssid / frequency / signal level / flags / ssid\n"
    "aa:bb:cc:dd:ee:ff\t2437\t-47\t[WPA2-PSK-CCMP][ESS]\tFRITZ!Box 7590\n"
    "11:22:33:44:55:66\t5180\t-63\t[WPA2-PSK-CCMP][SAE][ESS]\tWerkstatt 5G\n"
    "77:88:99:aa:bb:cc\t2412\t-80\t[ESS]\t\n"
    "de:ad:be:ef\t2412\t-70\t[ESS]\tzu kurz\n"
    "garbage line without tabs\n";

void test_scan_table()
{
    auto v = parse_scan_results(kScan);
    TCHECK(v.size() == 3);                       // the short bssid and the garbage are dropped
    TCHECK(v[0].ssid == "FRITZ!Box 7590");       // a space in the SSID survives
    TCHECK(v[0].signal_dbm == -47);
    TCHECK(v[0].frequency_mhz == 2437);
    TCHECK(v[1].ssid == "Werkstatt 5G");
    TCHECK(v[2].ssid.empty());                   // hidden network, still a result
}

void test_channels()
{
    TCHECK(channel_for_frequency(2412) == 1);
    TCHECK(channel_for_frequency(2437) == 6);
    TCHECK(channel_for_frequency(2472) == 13);
    TCHECK(channel_for_frequency(2484) == 14);
    TCHECK(channel_for_frequency(5180) == 36);
    TCHECK(channel_for_frequency(5825) == 165);
    TCHECK(channel_for_frequency(1234) == 0);
    TCHECK(channel_for_frequency(0) == 0);
}

void test_security_is_conservative()
{
    TCHECK(security_from_flags("[WPA2-PSK-CCMP][ESS]") == (int)WifiSecurity::Wpa2);
    TCHECK(security_from_flags("[WPA2-PSK-CCMP][SAE][ESS]") == (int)WifiSecurity::Wpa2Wpa3);
    TCHECK(security_from_flags("[SAE][ESS]") == (int)WifiSecurity::Wpa3);
    TCHECK(security_from_flags("[WEP][ESS]") == (int)WifiSecurity::Wep);
    TCHECK(security_from_flags("[ESS]") == (int)WifiSecurity::Open);
    // Unknown -> open, so the UI does not demand a passphrase nobody wants.
    TCHECK(security_from_flags("[SOMETHING-NEW][ESS]") == (int)WifiSecurity::Open);
}

void test_status_fields()
{
    const std::string st = "bssid=aa:bb:cc:dd:ee:ff\nssid=MyNet\nfreq=2437\nwpa_state=COMPLETED\nip_address=192.168.1.73\nempty=\n";
    std::string v;
    TCHECK(wpa_status_field(st, "ssid", v) && v == "MyNet");
    TCHECK(wpa_status_field(st, "wpa_state", v) && v == "COMPLETED");
    TCHECK(wpa_status_field(st, "ip_address", v) && v == "192.168.1.73");
    TCHECK(wpa_status_field(st, "empty", v) && v.empty());   // present but empty
    TCHECK(!wpa_status_field(st, "absent", v));              // is a different answer
    TCHECK(!wpa_status_field("", "ssid", v));
}

void test_empty_and_header_only()
{
    TCHECK(parse_scan_results("").empty());
    TCHECK(parse_scan_results("bssid / frequency / signal level / flags / ssid\n").empty());
}

} // namespace

void run_wpa_parse_tests()
{
    test_scan_table();
    test_channels();
    test_security_is_conservative();
    test_status_fields();
    test_empty_and_header_only();
}
