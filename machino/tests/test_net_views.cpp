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

// WLAN muss AUS sein, ohne dass irgendwer es abschaltet.
//
// Es gibt einen USB-Port. Ist WLAN an, laedt der Bootpfad cfg80211,
// aic_load_fw und aic8800 und hebt PB18 -- und dieser Treiber hat die Kamera
// beim Bring-up zweimal in den OOM getrieben. Eine Kamera, an der ein Modem
// haengen soll, darf das nicht bezahlen, nur weil niemand an die Einstellung
// gedacht hat. Deshalb steht der Default hier und nicht nur in einer Doku.
void test_usb_wifi_is_off_until_someone_says_otherwise()
{
    const usb::UsbConfig fresh;
    TCHECK(!fresh.wifi_enabled);

    // Auch ueber die Einstellungen: ein Gerät ohne den Schluessel in seiner
    // machino.conf -- also jede Kamera, die vor dieser Version installiert
    // wurde -- bleibt aus.
    usb::UsbConfig loaded;
    loaded.wifi_enabled = true;                   // als waere etwas anderes gesetzt
    std::string err;
    TCHECK(usb_config_from_settings({{"usb.enabled", "true"}}, loaded, err));
    TCHECK(loaded.wifi_enabled);                  // ein fehlender Schluessel aendert nichts
    usb::UsbConfig from_scratch;
    TCHECK(usb_config_from_settings({{"usb.enabled", "true"}}, from_scratch, err));
    TCHECK(!from_scratch.wifi_enabled);           // und der Ausgangspunkt ist aus

    TCHECK(usb_config_from_settings({{"usb.wifi.enabled", "true"}}, from_scratch, err));
    TCHECK(from_scratch.wifi_enabled);

    // Die Seite muss "Neustart erforderlich" sagen koennen, ohne es zu raten.
    const Json j = usb_config_json(fresh);
    const Json* w = j.get("wifi");
    TCHECK(w && w->is_object());
    TCHECK(w->get("enabled") && !w->get("enabled")->as_bool());
    TCHECK(w->get("appliesAt") && w->get("appliesAt")->as_string() == "reboot");
}

// Der Vertrag zwischen machino und dem Init-Skript, woertlich.
//
// S42wifi liest usb.wifi.enabled aus machino.conf, BEVOR machino laeuft -- es
// gibt keine API, die es fragen koennte. Damit haengt das Laden eines
// Kernelmoduls an der Textform einer Zeile, und die beiden Seiten sind in
// verschiedenen Sprachen geschrieben und werden nie zusammen ausgefuehrt.
//
// Genau diese Kette ist nirgends komplett gelaufen: die Kamera traegt einen
// machino von vor diesem Feld, und bei den Hardwaretests wurde der Schluessel
// von Hand geschrieben. Also wird hier die Zeile festgenagelt, die der
// ConfigStore erzeugt, und drueben in test_openipc_install.sh dieselbe Zeile
// durch den echten Parser des Init-Skripts geschickt. Treffen sich die beiden
// nicht mehr, faellt eine von beiden Seiten um.
void test_the_wifi_switch_is_written_the_way_the_init_script_reads_it()
{
    usb::UsbConfig cfg;
    cfg.wifi_enabled = true;
    std::vector<std::pair<std::string, std::string>> kv;
    usb_config_to_settings(cfg, kv);

    bool found = false;
    for (const auto& p : kv) {
        if (p.first != "usb.wifi.enabled") continue;
        found = true;
        // Kleingeschrieben: der Parser im Init-Skript akzeptiert bewusst kein
        // "TRUE" (fail-closed), also darf hier auch keins entstehen.
        TCHECK(p.second == "true");
    }
    TCHECK(found);

    cfg.wifi_enabled = false;
    kv.clear();
    usb_config_to_settings(cfg, kv);
    for (const auto& p : kv)
        if (p.first == "usb.wifi.enabled") TCHECK(p.second == "false");
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
    cfg.wifi_enabled = true;

    std::vector<std::pair<std::string, std::string>> kv;
    usb_config_to_settings(cfg, kv);

    usb::UsbConfig back;
    std::string err;
    TCHECK(usb_config_from_settings(kv, back, err));
    TCHECK(back.enabled == cfg.enabled);
    TCHECK(back.wifi_enabled == cfg.wifi_enabled);
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
    c.driver_ap_known = true; c.driver_ap = true;   // the chip could
    c.hostapd_available = false;        // but the image cannot
    c.wpa_supplicant_available = true;
    c.ap_unavailable_reason = "hostapd is not part of this image";

    const std::string txt = wifi_capabilities_json(c).dump();
    TCHECK(txt.find("\"accessPoint\":true") != std::string::npos);   // driverSupports
    TCHECK(txt.find("\"hostapd\":false") != std::string::npos);      // tooling
    TCHECK(txt.find("\"usable\":{\"station\":true,\"accessPoint\":false") != std::string::npos);
    TCHECK(txt.find("hostapd is not part of this image") != std::string::npos);
}

void test_an_unasked_driver_is_reported_as_unknown_not_as_no()
{
    // "We have not asked the radio" and "the radio said no" are different
    // answers, and collapsing them is a mistake this code made twice: first by
    // deriving driver_ap from the presence of a hostapd BINARY -- which is a
    // userspace package and proves nothing about the driver -- and then by
    // reporting the unasked case as a flat false.
    net::WifiCapabilities c;
    c.present = true;
    c.driver_station = c.driver_scan = true;
    c.wpa_supplicant_available = true;
    c.hostapd_available = true;
    c.dhcp_server_available = true;
    // driver_ap_known stays false: nothing has asked nl80211.

    const std::string txt = wifi_capabilities_json(c).dump();
    TCHECK(txt.find("\"accessPoint\":null") != std::string::npos);
    TCHECK(txt.find("\"accessPointKnown\":false") != std::string::npos);

    // Not verified -- so nothing may present it as a capability ...
    TCHECK(!c.ap_verified());
    TCHECK(txt.find("\"accessPointVerified\":false") != std::string::npos);
    // ... but attemptable, because refusing every board we have not
    // interrogated would make the feature unreachable on all of them.
    TCHECK(c.ap_attemptable());

    // And once the driver HAS said no, it is a no.
    c.driver_ap_known = true;
    c.driver_ap = false;
    TCHECK(!c.ap_attemptable());
    TCHECK(!c.ap_verified());
    TCHECK(wifi_capabilities_json(c).dump().find("\"accessPoint\":false") != std::string::npos);

    // A hostapd binary on its own still proves nothing.
    net::WifiCapabilities b;
    b.present = true;
    b.hostapd_available = true;
    TCHECK(!b.driver_ap_known);
    TCHECK(!b.ap_verified());
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

    // A one-character passphrase used to be accepted here. That was the gap,
    // not the expectation -- it reached wpa_supplicant and came back as a
    // generic connection failure. Now it is refused, so the shortest thing
    // that gets through is a real WPA passphrase.
    TCHECK(!wifi_station_from_json(parse("{\"ssid\":\"x\",\"passphrase\":\"y\"}"), cfg, err));

    TCHECK(wifi_station_from_json(parse("{\"ssid\":\"x\",\"passphrase\":\"longenough\"}"), cfg, err));
    TCHECK(cfg.ssid == "x" && cfg.dhcp);
}

void test_a_typo_is_an_error_not_a_silent_no_op()
{
    // Regression: every parser ignored unknown keys, so a PATCH with a
    // misspelled field answered 200 and changed nothing. The user sees success
    // and the setting is gone.
    std::string err;

    usb::UsbConfig u;
    TCHECK(!usb_config_from_json(parse("{\"enabledd\":true}"), u, err));
    TCHECK(err.find("enabledd") != std::string::npos);
    TCHECK(!usb_config_from_json(parse("{\"power\":{\"pinn\":\"PB18\"}}"), u, err));
    TCHECK(err.find("power.pinn") != std::string::npos);

    net::UplinkPolicy p;
    TCHECK(!policy_from_json(parse("{\"autoFailoverr\":false}"), p, err));
    TCHECK(p.auto_failover);                    // and nothing changed

    net::WifiStationConfig w;
    TCHECK(!wifi_station_from_json(parse("{\"ssid\":\"x\",\"pasword\":\"longenough\"}"), w, err));

    net::WifiApConfig a;
    TCHECK(!wifi_ap_from_json(parse("{\"ssid\":\"cam\",\"security\":\"open\",\"chanel\":6}"), a, err));

    // Der WLAN-Schalter entscheidet, ob beim naechsten Boot ein Treiber
    // geladen wird. Ein Tippfehler darf ihn nicht stillschweigend auf seinem
    // Default stehen lassen und trotzdem 200 antworten.
    TCHECK(!usb_config_from_json(parse("{\"wifi\":{\"enabledd\":true}}"), u, err));
    TCHECK(!usb_config_from_json(parse("{\"wify\":{\"enabled\":true}}"), u, err));
    TCHECK(!usb_config_from_json(parse("{\"wifi\":true}"), u, err));
    TCHECK(!usb_config_from_json(parse("{\"wifi\":{\"enabled\":\"yes\"}}"), u, err));
    // appliesAt wird BERICHTET, nicht angenommen: es ist eine Eigenschaft der
    // Einstellung, keine Wahl des Aufrufers.
    TCHECK(!usb_config_from_json(parse("{\"wifi\":{\"appliesAt\":\"now\"}}"), u, err));

    // The correctly spelled versions still work.
    TCHECK(usb_config_from_json(parse("{\"enabled\":true}"), u, err));
    TCHECK(usb_config_from_json(parse("{\"wifi\":{\"enabled\":true}}"), u, err));
    TCHECK(u.wifi_enabled);
    TCHECK(usb_config_from_json(parse("{\"wifi\":{\"enabled\":false}}"), u, err));
    TCHECK(!u.wifi_enabled);
    TCHECK(policy_from_json(parse("{\"autoFailover\":false}"), p, err));
    TCHECK(wifi_station_from_json(parse("{\"ssid\":\"x\",\"password\":\"x\"}"), w, err) == false);
    TCHECK(wifi_station_from_json(parse("{\"ssid\":\"x\",\"passphrase\":\"longenough\"}"), w, err));
}

void test_a_raw_hex_psk_is_accepted()
{
    // Regression: the field was capped at 63, so a valid 64-digit PSK pasted
    // from a router was rejected with "too long" and no way forward.
    const std::string hex64(64, 'a');
    net::WifiStationConfig cfg;
    std::string err;
    TCHECK(wifi_station_from_json(parse("{\"ssid\":\"x\",\"passphrase\":\"" + hex64 + "\"}"), cfg, err));
    TCHECK(cfg.passphrase.size() == 64);

    // 64 characters that are not hex is a mistake, not a key.
    const std::string notHex(63, 'z');
    TCHECK(!wifi_station_from_json(parse("{\"ssid\":\"x\",\"passphrase\":\"" + notHex + "z\"}"), cfg, err));
    TCHECK(err.find("hexadecimal") != std::string::npos);
}

void test_short_passphrase_is_refused_before_it_reaches_the_supplicant()
{
    // Otherwise wpa_supplicant rejects it later and the user sees "failed to
    // connect" instead of "your password is too short".
    net::WifiStationConfig cfg;
    std::string err;
    TCHECK(!wifi_station_from_json(parse("{\"ssid\":\"x\",\"passphrase\":\"abc\"}"), cfg, err));
    TCHECK(err.find("8 to 63") != std::string::npos);

    // An open network has none at all, and that is fine.
    TCHECK(wifi_station_from_json(parse("{\"ssid\":\"x\"}"), cfg, err));
}

void test_static_addresses_must_be_addresses()
{
    net::WifiStationConfig cfg;
    std::string err;
    TCHECK(!wifi_station_from_json(parse("{\"ssid\":\"x\",\"dhcp\":false,\"ip\":\"nicht-eine-ip\"}"), cfg, err));
    TCHECK(err.find("IPv4") != std::string::npos);

    TCHECK(!wifi_station_from_json(parse("{\"ssid\":\"x\",\"dhcp\":false,\"ip\":\"192.168.1.300\"}"), cfg, err));
    TCHECK(!wifi_station_from_json(parse("{\"ssid\":\"x\",\"dhcp\":false,\"ip\":\"192.168.01.1\"}"), cfg, err));
    TCHECK(!wifi_station_from_json(parse("{\"ssid\":\"x\",\"dhcp\":false,\"ip\":\"192.168.1.1 \"}"), cfg, err));
    TCHECK(!wifi_station_from_json(parse("{\"ssid\":\"x\",\"dhcp\":false,\"ip\":\"192.168.1\"}"), cfg, err));
    TCHECK(!wifi_station_from_json(parse("{\"ssid\":\"x\",\"ip\":\"1.2.3.4\",\"gateway\":\"gw\"}"), cfg, err));

    TCHECK(wifi_station_from_json(
        parse("{\"ssid\":\"x\",\"dhcp\":false,\"ip\":\"192.168.1.73\",\"netmask\":\"255.255.255.0\","
              "\"gateway\":\"192.168.1.1\",\"dns\":\"9.9.9.9\"}"), cfg, err));
    TCHECK(cfg.static_ip == "192.168.1.73" && !cfg.dhcp);
}

void test_ap_addresses_are_validated_too()
{
    net::WifiApConfig cfg;
    std::string err;
    TCHECK(!wifi_ap_from_json(parse("{\"ssid\":\"cam\",\"security\":\"open\",\"ip\":\"999.1.1.1\"}"), cfg, err));
    TCHECK(!wifi_ap_from_json(parse("{\"ssid\":\"cam\",\"security\":\"open\",\"dhcpStart\":\"x\"}"), cfg, err));
    TCHECK(wifi_ap_from_json(parse("{\"ssid\":\"cam\",\"security\":\"open\",\"ip\":\"192.168.4.1\"}"), cfg, err));
}

void test_secured_ap_needs_a_real_passphrase()
{
    net::WifiApConfig cfg;
    std::string err;
    TCHECK(!wifi_ap_from_json(parse("{\"ssid\":\"cam\",\"security\":\"wpa2\",\"passphrase\":\"short\"}"), cfg, err));
    TCHECK(err.find("8 to 63") != std::string::npos);

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

namespace {

void test_policy_survives_a_settings_round_trip()
{
    net::UplinkPolicy p;
    p.order = {"lte1", "ethernet", "wifi"};
    p.auto_failover = false;
    p.return_to_preferred = false;
    p.pinned = true;
    p.pinned_uplink = "lte1";

    std::vector<std::pair<std::string, std::string>> kv;
    policy_to_settings(p, kv);

    net::UplinkPolicy back;
    std::string err;
    TCHECK(policy_from_settings(kv, back, err));
    TCHECK(back.order == p.order);
    TCHECK(back.auto_failover == false && back.return_to_preferred == false);
    TCHECK(back.pinned && back.pinned_uplink == "lte1");
}

void test_an_empty_order_in_the_file_leaves_the_default_alone()
{
    // A present-but-empty key is "unset", not "no preference at all". Clearing
    // the order would leave the camera with nothing to choose between.
    net::UplinkPolicy p;
    const std::vector<std::string> before = p.order;
    std::vector<std::pair<std::string, std::string>> kv{{"network.order", ""}};
    std::string err;
    TCHECK(policy_from_settings(kv, p, err));
    TCHECK(p.order == before);
}

void test_the_order_list_tolerates_spacing()
{
    net::UplinkPolicy p;
    std::vector<std::pair<std::string, std::string>> kv{{"network.order", " wifi , ethernet ,, cellular "}};
    std::string err;
    TCHECK(policy_from_settings(kv, p, err));
    TCHECK(p.order.size() == 3);
    TCHECK(p.order.size() == 3 && p.order[0] == "wifi" && p.order[2] == "cellular");
}

void test_a_hand_edited_pin_to_nothing_is_refused_too()
{
    // The file must not be able to produce a state the API would reject.
    net::UplinkPolicy p;
    std::vector<std::pair<std::string, std::string>> kv{{"network.pinned", "true"}};
    std::string err;
    TCHECK(!policy_from_settings(kv, p, err));
    TCHECK(!p.pinned);
}

} // namespace

void run_net_views_tests()
{
    test_policy_survives_a_settings_round_trip();
    test_an_empty_order_in_the_file_leaves_the_default_alone();
    test_the_order_list_tolerates_spacing();
    test_a_hand_edited_pin_to_nothing_is_refused_too();
    test_usb_status_reports_capabilities_and_resolution();
    test_unknown_power_state_is_not_reported_as_off();
    test_usb_patch_rejects_bad_values_and_changes_nothing();
    test_usb_config_survives_a_settings_round_trip();
    test_usb_wifi_is_off_until_someone_says_otherwise();
    test_the_wifi_switch_is_written_the_way_the_init_script_reads_it();
    test_wifi_capabilities_separate_driver_from_tooling();
    test_an_unasked_driver_is_reported_as_unknown_not_as_no();
    test_no_document_ever_contains_a_passphrase();
    test_wifi_connect_requires_an_ssid();
    test_a_typo_is_an_error_not_a_silent_no_op();
    test_a_raw_hex_psk_is_accepted();
    test_short_passphrase_is_refused_before_it_reaches_the_supplicant();
    test_static_addresses_must_be_addresses();
    test_ap_addresses_are_validated_too();
    test_secured_ap_needs_a_real_passphrase();
    test_policy_patch_refuses_a_pin_to_nothing();
    test_network_document_names_the_active_uplink();
}
