// The USB and connectivity API surface, as documents.
//
// Two properties are worth more than the field-by-field checks below and are
// asserted explicitly: a passphrase never reaches a response, and every
// document reports capabilities so a UI can grey out what the board cannot do
// rather than offering a control that fails.
#include "app/api/net_views.hpp"
#include <cstdio>
#include <string>

using namespace machino;
using namespace machino::api;

extern int g_fail_ext, g_pass_ext;
#define TCHECK(cond) do { if (cond) { ++g_pass_ext; } else { ++g_fail_ext; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

namespace {

Json parse(const std::string& text)
{
    Json j; std::string err;
    Json::parse(text, j, err);
    return j;
}

UsbCapabilities board()
{
    UsbCapabilities c;
    c.host_supported = true;
    c.controller = "dwc2";
    c.max_speed = "high";
    c.power.switchable = true;
    c.power.default_pin = "PB18";
    c.power.default_active_high = true;
    c.power.voltage_mv = 3300;
    c.power.allowed_pins = {"PB18"};
    return c;
}

void test_usb_status_reports_capabilities_and_resolution()
{
    usb::UsbStatus s;
    s.enabled = true;
    s.host_active = true;
    s.caps = board();
    s.resolved.mode = UsbPowerMode::Gpio;
    s.resolved.pin = "PB18";
    s.resolved.active_high = true;
    s.resolved.drives_power = true;
    s.power_known = true;
    s.power_on = true;

    UsbDevice d;
    d.path = "1-1"; d.vid = "a69c"; d.pid = "88dc";
    d.manufacturer = "AICSemi"; d.product = "AIC8800DC"; d.speed = "480"; d.max_power_ma = 500;
    UsbInterface i; i.cls = "ff"; i.subclass = "ff"; i.protocol = "ff";
    d.interfaces.push_back(i);
    s.devices.push_back(d);

    const std::string txt = usb_status_json(s).dump();
    TCHECK(txt.find("\"hostActive\":true") != std::string::npos);
    TCHECK(txt.find("\"pin\":\"PB18\"") != std::string::npos);
    TCHECK(txt.find("\"state\":\"on\"") != std::string::npos);
    TCHECK(txt.find("\"voltageMv\":3300") != std::string::npos);
    TCHECK(txt.find("a69c") != std::string::npos && txt.find("AIC8800DC") != std::string::npos);
    TCHECK(txt.find("\"allowedPins\":[\"PB18\"]") != std::string::npos);
}

void test_unknown_power_state_is_not_reported_as_off()
{
    // A hard-wired rail cannot be read back. Saying "off" would look like a
    // fault; "unknown" is the honest answer.
    usb::UsbStatus s;
    s.caps = board();
    s.power_known = false;
    TCHECK(usb_status_json(s).dump().find("\"state\":\"unknown\"") != std::string::npos);
}

void test_usb_patch_rejects_bad_values_and_changes_nothing()
{
    usb::UsbConfig cfg;
    cfg.pin = "PB18";
    const usb::UsbConfig before = cfg;
    std::string err;

    TCHECK(!usb_config_from_json(parse("{\"power\":{\"mode\":\"sometimes\"}}"), cfg, err));
    TCHECK(err.find("board-default") != std::string::npos);
    TCHECK(cfg.pin == before.pin && cfg.mode == before.mode);

    TCHECK(!usb_config_from_json(parse("{\"enabled\":\"yes\"}"), cfg, err));
    TCHECK(!usb_config_from_json(parse("{\"power\":{\"activeLevel\":\"middling\"}}"), cfg, err));
    TCHECK(!usb_config_from_json(parse("[]"), cfg, err));

    TCHECK(usb_config_from_json(parse("{\"enabled\":true,\"power\":{\"mode\":\"gpio\",\"activeLevel\":\"low\"}}"), cfg, err));
    TCHECK(cfg.enabled && cfg.mode == UsbPowerMode::Gpio && !cfg.active_high);
}

void test_usb_config_survives_a_settings_round_trip()
{
    usb::UsbConfig cfg;
    cfg.enabled = true;
    cfg.mode = UsbPowerMode::Gpio;
    cfg.pin = "PB18";
    cfg.active_high = false;
    cfg.enable_at_boot = false;
    cfg.expert = true;

    std::vector<std::pair<std::string, std::string>> kv;
    usb_config_to_settings(cfg, kv);

    usb::UsbConfig back;
    std::string err;
    TCHECK(usb_config_from_settings(kv, back, err));
    TCHECK(back.enabled == cfg.enabled);
    TCHECK(back.mode == cfg.mode);
    TCHECK(back.pin == cfg.pin);
    TCHECK(back.active_high == cfg.active_high);
    TCHECK(back.enable_at_boot == cfg.enable_at_boot);
    TCHECK(back.expert == cfg.expert);

    // A nonsense value in the file is an error, not a silent default.
    std::vector<std::pair<std::string, std::string>> bad{{"usb.power.mode", "sideways"}};
    usb::UsbConfig c2;
    TCHECK(!usb_config_from_settings(bad, c2, err));
}

void test_wifi_capabilities_separate_driver_from_tooling()
{
    net::WifiCapabilities c;
    c.present = true;
    c.driver_station = true;
    c.driver_scan = true;
    c.driver_ap = true;                 // the chip could
    c.hostapd_available = false;        // but the image cannot
    c.wpa_supplicant_available = true;
    c.ap_unavailable_reason = "hostapd is not part of this image";

    const std::string txt = wifi_capabilities_json(c).dump();
    TCHECK(txt.find("\"accessPoint\":true") != std::string::npos);   // driverSupports
    TCHECK(txt.find("\"hostapd\":false") != std::string::npos);      // tooling
    TCHECK(txt.find("\"usable\":{\"station\":true,\"accessPoint\":false") != std::string::npos);
    TCHECK(txt.find("hostapd is not part of this image") != std::string::npos);
}

void test_no_document_ever_contains_a_passphrase()
{
    net::WifiCapabilities c;
    c.present = true; c.driver_station = true; c.wpa_supplicant_available = true;
    net::WifiNetwork n;
    n.ssid = "FRITZ!Box"; n.bssid = "aa:bb:cc:dd:ee:ff"; n.channel = 6; n.rssi_dbm = -47;
    n.security = net::WifiSecurity::Wpa2;

    const std::string status = wifi_status_json(c, net::WifiMode::Station, net::LinkState::Connected, &n).dump();
    const std::string scan = wifi_scan_json({n}).dump();

    for (const std::string& doc : {status, scan}) {
        TCHECK(doc.find("passphrase") == std::string::npos);
        TCHECK(doc.find("password") == std::string::npos);
        TCHECK(doc.find("psk") == std::string::npos);
    }
    TCHECK(status.find("FRITZ!Box") != std::string::npos);   // the SSID is fine
}

void test_wifi_connect_requires_an_ssid()
{
    net::WifiStationConfig cfg;
    std::string err;
    TCHECK(!wifi_station_from_json(parse("{\"passphrase\":\"secret\"}"), cfg, err));
    TCHECK(err.find("ssid") != std::string::npos);

    // A static configuration without an address is a request to be unreachable.
    TCHECK(!wifi_station_from_json(parse("{\"ssid\":\"x\",\"dhcp\":false}"), cfg, err));

    TCHECK(wifi_station_from_json(parse("{\"ssid\":\"x\",\"passphrase\":\"y\"}"), cfg, err));
    TCHECK(cfg.ssid == "x" && cfg.dhcp);
}

void test_secured_ap_needs_a_real_passphrase()
{
    net::WifiApConfig cfg;
    std::string err;
    TCHECK(!wifi_ap_from_json(parse("{\"ssid\":\"cam\",\"security\":\"wpa2\",\"passphrase\":\"short\"}"), cfg, err));
    TCHECK(err.find("8 characters") != std::string::npos);

    TCHECK(wifi_ap_from_json(parse("{\"ssid\":\"cam\",\"security\":\"open\"}"), cfg, err));
    TCHECK(cfg.security == net::WifiSecurity::Open);

    TCHECK(wifi_ap_from_json(parse("{\"ssid\":\"cam\",\"security\":\"wpa2\",\"passphrase\":\"longenough\"}"), cfg, err));
    TCHECK(!wifi_ap_from_json(parse("{\"ssid\":\"cam\",\"channel\":9999}"), cfg, err));
    TCHECK(!wifi_ap_from_json(parse("{}"), cfg, err));
}

void test_policy_patch_refuses_a_pin_to_nothing()
{
    net::UplinkPolicy p;
    std::string err;
    TCHECK(!policy_from_json(parse("{\"pinned\":true}"), p, err));
    TCHECK(err.find("pinnedUplink") != std::string::npos);
    TCHECK(!p.pinned);

    TCHECK(!policy_from_json(parse("{\"order\":[]}"), p, err));
    TCHECK(!policy_from_json(parse("{\"order\":[1,2]}"), p, err));

    TCHECK(policy_from_json(parse("{\"order\":[\"wifi\",\"ethernet\"],\"autoFailover\":false}"), p, err));
    TCHECK(p.order.size() == 2 && p.order[0] == "wifi" && !p.auto_failover);
}

void test_network_document_names_the_active_uplink()
{
    net::UplinkStatus a;
    a.id = "eth0"; a.type = net::UplinkType::Ethernet; a.state = net::LinkState::Connected;
    a.internet = true; a.active = true; a.info.ifname = "eth0"; a.info.ipv4 = "192.168.1.10";
    net::UplinkStatus b;
    b.id = "wlan0"; b.type = net::UplinkType::Wifi; b.state = net::LinkState::Down;

    net::UplinkPolicy p;
    const std::string txt = network_json({a, b}, p, "eth0").dump();
    TCHECK(txt.find("\"activeUplink\":\"eth0\"") != std::string::npos);
    TCHECK(txt.find("\"id\":\"wlan0\"") != std::string::npos);
    TCHECK(txt.find("\"state\":\"down\"") != std::string::npos);
    TCHECK(txt.find("192.168.1.10") != std::string::npos);
}

} // namespace

void run_net_views_tests()
{
    test_usb_status_reports_capabilities_and_resolution();
    test_unknown_power_state_is_not_reported_as_off();
    test_usb_patch_rejects_bad_values_and_changes_nothing();
    test_usb_config_survives_a_settings_round_trip();
    test_wifi_capabilities_separate_driver_from_tooling();
    test_no_document_ever_contains_a_passphrase();
    test_wifi_connect_requires_an_ssid();
    test_secured_ap_needs_a_real_passphrase();
    test_policy_patch_refuses_a_pin_to_nothing();
    test_network_document_names_the_active_uplink();
}
