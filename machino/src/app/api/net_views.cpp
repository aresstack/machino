#include "app/api/net_views.hpp"

#include <algorithm>
#include <cstdlib>

namespace machino { namespace api {

namespace {

bool get_bool(const Json& o, const char* k, bool& out, std::string& err)
{
    const Json* v = o.get(k);
    if (!v) return true;                       // absent = leave alone
    if (!v->is_bool()) { err = std::string(k) + " must be true or false"; return false; }
    out = v->as_bool();
    return true;
}

bool get_string(const Json& o, const char* k, std::string& out, std::string& err, size_t max = 256)
{
    const Json* v = o.get(k);
    if (!v) return true;
    if (!v->is_string()) { err = std::string(k) + " must be a string"; return false; }
    if (v->as_string().size() > max) { err = std::string(k) + " is too long"; return false; }
    out = v->as_string();
    return true;
}

bool get_int(const Json& o, const char* k, int& out, int lo, int hi, std::string& err)
{
    const Json* v = o.get(k);
    if (!v) return true;
    if (!v->is_number()) { err = std::string(k) + " must be a number"; return false; }
    const double d = v->as_number();
    if (d < lo || d > hi) { err = std::string(k) + " is out of range"; return false; }
    out = (int)d;
    return true;
}

// An unknown key is an error, not something to skip.
//
// Silently ignoring it means a PATCH with a typo answers 200 and changes
// nothing: the user sees success and the setting is gone. The same rule is
// already enforced in the weirdiked config parser for the same reason -- a
// misspelling must not leave a security-relevant setting at its default.
bool reject_unknown(const Json& o, std::initializer_list<const char*> allowed,
                    const char* where, std::string& err)
{
    for (const auto& m : o.members()) {
        bool known = false;
        for (const char* a : allowed) if (m.first == a) { known = true; break; }
        if (!known) {
            err = std::string("unknown field: ") + (where ? std::string(where) + "." : std::string()) + m.first;
            return false;
        }
    }
    return true;
}

Json string_array(const std::vector<std::string>& v)
{
    Json a = Json::array();
    for (const auto& s : v) a.push(Json::string(s));
    return a;
}

// Dotted quad, strictly. Rejects leading zeros, out-of-range octets and
// trailing junk. Without this a static configuration could be handed a string
// like "nicht-eine-ip", which then lands in a network config file and the
// camera comes up unreachable with no explanation.
bool valid_ipv4(const std::string& s)
{
    int octets = 0;
    size_t i = 0;
    while (octets < 4) {
        size_t start = i;
        int v = 0, digits = 0;
        while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
            v = v * 10 + (s[i] - '0');
            if (++digits > 3 || v > 255) return false;
            ++i;
        }
        if (digits == 0) return false;
        if (digits > 1 && s[start] == '0') return false;
        ++octets;
        if (octets < 4) {
            if (i >= s.size() || s[i] != '.') return false;
            ++i;
        }
    }
    return i == s.size();
}

bool check_optional_ipv4(const std::string& v, const char* field, std::string& err)
{
    if (v.empty()) return true;
    if (valid_ipv4(v)) return true;
    err = std::string(field) + " is not an IPv4 address";
    return false;
}

// WPA accepts either a passphrase of 8..63 characters or a raw 64-hex-digit
// PSK. An earlier version capped the field at 63, which rejected the perfectly
// valid raw key -- someone pasting one from their router got "too long" and no
// way forward.
bool check_wifi_secret(const std::string& s, bool required, std::string& err)
{
    if (s.empty()) {
        if (!required) return true;
        err = "a secured network needs a passphrase";
        return false;
    }
    if (s.size() == 64) {
        for (char c : s) {
            const bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
            if (!hex) { err = "a 64-character key must be a hexadecimal PSK"; return false; }
        }
        return true;
    }
    if (s.size() < 8 || s.size() > 63) {
        err = "a passphrase must be 8 to 63 characters, or a 64-digit hexadecimal PSK";
        return false;
    }
    return true;
}

// Ein Wert, der spaeter in eine Konfigurationsdatei oder ein Chat-Skript geht,
// darf dort keine Zeile beenden und kein eigenes Wort anfangen.
//
// Die Einwahlnummer landet zwischen EINFACHEN Anfuehrungszeichen im
// Chat-Skript (`OK 'ATD*99***1#'`), APN und Benutzername zwischen DOPPELTEN in
// der pppd-Optionsdatei. Ein Anfuehrungszeichen im Wert bricht dort aus, und
// aus einem Formularfeld wird eine Anweisung -- zum Beispiel `defaultroute`,
// genau die Option, die machino unterdrueckt, weil sie dem Failover die
// Grundlage naehme.
//
// Abgewiesen statt maskiert: was hier nicht hineingehoert, hat auch keine
// sinnvolle maskierte Form. Dieselbe Pruefung steht noch einmal im Backend --
// dort als zweite Linie, weil dieser Pfad auch aus einer von Hand bearbeiteten
// machino.conf erreichbar ist.
bool config_safe(const std::string& s, const char* field, std::string& err)
{
    for (char c : s) {
        if (c == '\n' || c == '\r' || c == '"' || c == '\'' || c == '\\' ||
            (unsigned char)c < 0x20) {
            err = std::string(field) + " must not contain quotes, backslashes or line breaks";
            return false;
        }
    }
    return true;
}

} // namespace

// ------------------------------------------------------------------ USB

Json usb_capabilities_json(const UsbCapabilities& c)
{
    Json j = Json::object();
    j.set("hostSupported", Json::boolean(c.host_supported));
    j.set("controller", Json::string(c.controller));
    j.set("maxSpeed", Json::string(c.max_speed));

    Json p = Json::object();
    p.set("switchable", Json::boolean(c.power.switchable));
    p.set("defaultPin", Json::string(c.power.default_pin));
    p.set("defaultActiveHigh", Json::boolean(c.power.default_active_high));
    p.set("voltageMv", Json::integer(c.power.voltage_mv));
    p.set("allowedPins", string_array(c.power.allowed_pins));
    j.set("power", p);
    return j;
}

Json usb_config_json(const usb::UsbConfig& cfg)
{
    Json j = Json::object();
    j.set("enabled", Json::boolean(cfg.enabled));
    Json p = Json::object();
    p.set("mode", Json::string(usb_power_mode_name(cfg.mode)));
    p.set("pin", Json::string(cfg.pin));
    p.set("activeLevel", Json::string(cfg.active_high ? "high" : "low"));
    p.set("enableAtBoot", Json::boolean(cfg.enable_at_boot));
    p.set("expert", Json::boolean(cfg.expert));
    j.set("power", p);
    // ONE selector for the one port. The page has to be able to say "restart
    // required" without hard-coding that knowledge, and a client that only
    // reads the API should not have to know which settings are live.
    j.set("mode", Json::string(usb::usb_function_name(cfg.function)));
    j.set("modeAppliesAt", Json::string("reboot"));
    return j;
}

Json usb_devices_json(const std::vector<UsbDevice>& devices)
{
    Json a = Json::array();
    for (const UsbDevice& d : devices) {
        Json o = Json::object();
        o.set("path", Json::string(d.path));
        o.set("vid", Json::string(d.vid));
        o.set("pid", Json::string(d.pid));
        o.set("manufacturer", Json::string(d.manufacturer));
        o.set("product", Json::string(d.product));
        o.set("speed", Json::string(d.speed));
        o.set("maxPowerMa", Json::integer(d.max_power_ma));
        Json ifs = Json::array();
        for (const UsbInterface& i : d.interfaces) {
            Json io = Json::object();
            io.set("class", Json::string(i.cls));
            io.set("subclass", Json::string(i.subclass));
            io.set("protocol", Json::string(i.protocol));
            io.set("driver", Json::string(i.driver));
            ifs.push(io);
        }
        o.set("interfaces", ifs);
        a.push(o);
    }
    return a;
}

Json usb_status_json(const usb::UsbStatus& s)
{
    Json j = Json::object();
    j.set("enabled", Json::boolean(s.enabled));
    j.set("hostActive", Json::boolean(s.host_active));

    // Was gespeichert ist, was beim Boot tatsaechlich gestartet wurde, und ob
    // das auseinanderfaellt. Genau dazwischen liegt der Neustart, und nur
    // deshalb hat die Oberflaeche ueberhaupt etwas zu sagen.
    j.set("mode", Json::string(usb::usb_function_name(s.function)));
    j.set("bootMode", Json::string(usb::usb_function_name(s.boot_function)));
    j.set("rebootRequired", Json::boolean(s.reboot_required));

    Json p = Json::object();
    p.set("mode", Json::string(usb_power_mode_name(s.resolved.mode)));
    p.set("pin", Json::string(s.resolved.pin));
    p.set("activeLevel", Json::string(s.resolved.active_high ? "high" : "low"));
    p.set("drivesPower", Json::boolean(s.resolved.drives_power));
    // "unknown" is a real answer -- a hard-wired rail cannot be read back, and
    // reporting false there would look like a fault.
    p.set("state", Json::string(!s.power_known ? "unknown" : (s.power_on ? "on" : "off")));
    j.set("power", p);

    j.set("capabilities", usb_capabilities_json(s.caps));
    j.set("devices", usb_devices_json(s.devices));
    return j;
}

bool usb_config_from_json(const Json& body, usb::UsbConfig& cfg, std::string& err)
{
    if (!body.is_object()) { err = "body must be an object"; return false; }

    usb::UsbConfig next = cfg;
    // modeAppliesAt is reported, not accepted: it is a property of the
    // setting, not something a client gets to choose.
    if (!reject_unknown(body, {"enabled", "power", "mode"}, nullptr, err)) return false;
    if (!get_bool(body, "enabled", next.enabled, err)) return false;

    std::string mode_text;
    if (!get_string(body, "mode", mode_text, err, 16)) return false;
    if (!mode_text.empty() && !usb::usb_function_parse(mode_text, next.function)) {
        err = "mode must be off, wifi or cellular";
        return false;
    }

    const Json* p = body.get("power");
    if (p) {
        if (!p->is_object()) { err = "power must be an object"; return false; }
        if (!reject_unknown(*p, {"mode", "pin", "activeLevel", "enableAtBoot", "expert"}, "power", err)) return false;
        std::string mode;
        if (!get_string(*p, "mode", mode, err, 32)) return false;
        if (!mode.empty() && !usb_power_mode_parse(mode, next.mode)) {
            err = "power.mode must be board-default, gpio, always-on or none";
            return false;
        }
        if (!get_string(*p, "pin", next.pin, err, 32)) return false;
        std::string level;
        if (!get_string(*p, "activeLevel", level, err, 8)) return false;
        if (!level.empty()) {
            if (level != "high" && level != "low") { err = "power.activeLevel must be high or low"; return false; }
            next.active_high = (level == "high");
        }
        if (!get_bool(*p, "enableAtBoot", next.enable_at_boot, err)) return false;
        if (!get_bool(*p, "expert", next.expert, err)) return false;
    }

    cfg = next;
    return true;
}

void usb_config_to_settings(const usb::UsbConfig& cfg,
                            std::vector<std::pair<std::string, std::string>>& out)
{
    out.emplace_back("usb.enabled", cfg.enabled ? "true" : "false");
    out.emplace_back("usb.power.mode", usb_power_mode_name(cfg.mode));
    out.emplace_back("usb.power.pin", cfg.pin);
    out.emplace_back("usb.power.active_level", cfg.active_high ? "high" : "low");
    out.emplace_back("usb.power.enable_at_boot", cfg.enable_at_boot ? "true" : "false");
    out.emplace_back("usb.power.expert", cfg.expert ? "true" : "false");
    out.emplace_back("usb.mode", usb::usb_function_name(cfg.function));
    // usb.wifi.enabled is written out as well, and ONLY as a mirror.
    //
    // It is what the boot helper of an older installation reads. Dropping it
    // on the first save would leave such a camera with a mode nothing acts on:
    // the new key is there, the old script does not know it, and WiFi silently
    // stops coming up after an upgrade that did not replace the init script.
    // The mirror is derived, never read back as truth -- see
    // usb_config_from_settings.
    out.emplace_back("usb.wifi.enabled",
                     cfg.function == usb::UsbFunction::Wifi ? "true" : "false");
}

bool usb_config_from_settings(const std::vector<std::pair<std::string, std::string>>& in,
                              usb::UsbConfig& cfg, std::string& err)
{
    usb::UsbConfig next = cfg;

    // The migration, and it happens here because this is the only place that
    // sees the file.
    //
    // usb.mode is the truth. usb.wifi.enabled is what an installation from
    // before AP-M6 has instead, and it is honoured ONLY when usb.mode is
    // absent -- otherwise an old mirror left in the file would keep
    // overruling the new setting, and "I selected cellular and it came back
    // as WiFi" is the kind of bug nobody finds by reading the UI.
    //
    // One-way and one-time: the next save writes usb.mode, and from then on
    // the legacy key is a mirror nobody reads.
    bool have_mode = false;
    bool legacy_wifi = false, have_legacy = false;

    for (const auto& kv : in) {
        const std::string& k = kv.first;
        const std::string& v = kv.second;
        if      (k == "usb.enabled")               next.enabled = (v == "true" || v == "1");
        else if (k == "usb.wifi.enabled")          { legacy_wifi = (v == "true" || v == "1"); have_legacy = true; }
        else if (k == "usb.mode") {
            if (!usb::usb_function_parse(v, next.function)) {
                // Refused, not defaulted. Silently reading usb.mode=wlan as
                // "off" looks exactly like a setting that did not take, and
                // sends the owner looking in the wrong place. The caller keeps
                // what it had -- which on a fresh start is off, so the boot
                // path still fails closed.
                err = "usb.mode must be off, wifi or cellular, not: " + v;
                return false;
            }
            have_mode = true;
        }
        else if (k == "usb.power.pin")             next.pin = v;
        else if (k == "usb.power.enable_at_boot")  next.enable_at_boot = (v == "true" || v == "1");
        else if (k == "usb.power.expert")          next.expert = (v == "true" || v == "1");
        else if (k == "usb.power.active_level") {
            if (v != "high" && v != "low") { err = "usb.power.active_level must be high or low"; return false; }
            next.active_high = (v == "high");
        } else if (k == "usb.power.mode") {
            if (!usb_power_mode_parse(v, next.mode)) { err = "usb.power.mode is not a known mode: " + v; return false; }
        }
        // Unknown keys are someone else's; the config store keeps them.
    }

    if (!have_mode && have_legacy)
        next.function = legacy_wifi ? usb::UsbFunction::Wifi : usb::UsbFunction::Off;

    cfg = next;
    return true;
}

// --------------------------------------------------------- connectivity

Json uplink_status_json(const net::UplinkStatus& u)
{
    Json o = Json::object();
    o.set("id", Json::string(u.id));
    o.set("type", Json::string(net::uplink_type_name(u.type)));
    o.set("state", Json::string(net::link_state_name(u.state)));
    o.set("internet", Json::boolean(u.internet));
    o.set("active", Json::boolean(u.active));
    o.set("interface", Json::string(u.info.ifname));
    o.set("ipv4", Json::string(u.info.ipv4));
    o.set("netmask", Json::string(u.info.netmask));
    o.set("gateway", Json::string(u.info.gateway));
    o.set("dns", Json::string(u.info.dns));
    o.set("dhcp", Json::boolean(u.info.dhcp));
    Json m = Json::object();
    m.set("carrier", Json::boolean(u.metrics.carrier));
    m.set("rssiDbm", Json::integer(u.metrics.rssi_dbm));
    m.set("linkMbit", Json::integer(u.metrics.link_mbit));
    m.set("rxBytes", Json::integer((long long)u.metrics.rx_bytes));
    m.set("txBytes", Json::integer((long long)u.metrics.tx_bytes));
    o.set("metrics", m);
    return o;
}

Json policy_json(const net::UplinkPolicy& p)
{
    Json o = Json::object();
    o.set("order", string_array(p.order));
    o.set("autoFailover", Json::boolean(p.auto_failover));
    o.set("returnToPreferred", Json::boolean(p.return_to_preferred));
    o.set("pinned", Json::boolean(p.pinned));
    o.set("pinnedUplink", Json::string(p.pinned_uplink));
    return o;
}

Json network_json(const std::vector<net::UplinkStatus>& uplinks,
                  const net::UplinkPolicy& policy,
                  const std::string& active_id)
{
    Json j = Json::object();
    j.set("activeUplink", Json::string(active_id));
    Json a = Json::array();
    for (const auto& u : uplinks) a.push(uplink_status_json(u));
    j.set("uplinks", a);
    j.set("policy", policy_json(policy));
    return j;
}

bool policy_from_json(const Json& body, net::UplinkPolicy& p, std::string& err)
{
    if (!body.is_object()) { err = "body must be an object"; return false; }
    net::UplinkPolicy next = p;
    if (!reject_unknown(body, {"order", "autoFailover", "returnToPreferred", "pinned", "pinnedUplink"}, nullptr, err)) return false;

    const Json* ord = body.get("order");
    if (ord) {
        if (!ord->is_array()) { err = "order must be an array"; return false; }
        std::vector<std::string> v;
        for (size_t i = 0; i < ord->size(); ++i) {
            const Json& e = ord->at(i);
            if (!e.is_string() || e.as_string().empty()) { err = "order entries must be non-empty strings"; return false; }
            v.push_back(e.as_string());
        }
        if (v.empty()) { err = "order must not be empty"; return false; }
        next.order = v;
    }
    if (!get_bool(body, "autoFailover", next.auto_failover, err)) return false;
    if (!get_bool(body, "returnToPreferred", next.return_to_preferred, err)) return false;
    if (!get_bool(body, "pinned", next.pinned, err)) return false;
    if (!get_string(body, "pinnedUplink", next.pinned_uplink, err, 64)) return false;
    if (next.pinned && next.pinned_uplink.empty()) {
        err = "pinned needs pinnedUplink -- pinning to nothing would silently disable the network";
        return false;
    }

    p = next;
    return true;
}

void policy_to_settings(const net::UplinkPolicy& p,
                        std::vector<std::pair<std::string, std::string>>& out)
{
    std::string order;
    for (const std::string& s : p.order) {
        if (!order.empty()) order += ",";
        order += s;
    }
    out.emplace_back("network.order", order);
    out.emplace_back("network.auto_failover", p.auto_failover ? "true" : "false");
    out.emplace_back("network.return_to_preferred", p.return_to_preferred ? "true" : "false");
    out.emplace_back("network.pinned", p.pinned ? "true" : "false");
    out.emplace_back("network.pinned_uplink", p.pinned_uplink);
}

bool policy_from_settings(const std::vector<std::pair<std::string, std::string>>& in,
                          net::UplinkPolicy& p, std::string& err)
{
    net::UplinkPolicy next = p;
    for (const auto& kv : in) {
        const std::string& k = kv.first;
        const std::string& v = kv.second;
        if (k == "network.order") {
            // An empty value means the key is present but unset. Clearing the
            // order would leave the camera with no preference at all, so the
            // built-in default stands rather than being overwritten with
            // nothing.
            if (v.empty()) continue;
            std::vector<std::string> list;
            size_t pos = 0;
            while (pos <= v.size()) {
                const size_t comma = v.find(',', pos);
                std::string item = v.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
                size_t a = 0, b = item.size();
                while (a < b && item[a] == ' ') ++a;
                while (b > a && item[b - 1] == ' ') --b;
                item = item.substr(a, b - a);
                if (!item.empty()) list.push_back(item);
                if (comma == std::string::npos) break;
                pos = comma + 1;
            }
            if (list.empty()) { err = "network.order lists no usable entry"; return false; }
            next.order = list;
        }
        else if (k == "network.auto_failover")       next.auto_failover = (v == "true" || v == "1");
        else if (k == "network.return_to_preferred") next.return_to_preferred = (v == "true" || v == "1");
        else if (k == "network.pinned")              next.pinned = (v == "true" || v == "1");
        else if (k == "network.pinned_uplink")       next.pinned_uplink = v;
        // Unknown keys are someone else's; the config store keeps them.
    }

    // The same rule the JSON path enforces, because a hand-edited file must
    // not be able to produce a state the API refuses to create.
    if (next.pinned && next.pinned_uplink.empty()) {
        err = "network.pinned needs network.pinned_uplink";
        return false;
    }
    p = next;
    return true;
}

// ------------------------------------------------------------------ WiFi

Json wifi_capabilities_json(const net::WifiCapabilities& c)
{
    Json j = Json::object();
    j.set("present", Json::boolean(c.present));
    j.set("interface", Json::string(c.ifname));
    j.set("driver", Json::string(c.driver));

    Json d = Json::object();
    d.set("station", Json::boolean(c.driver_station));
    // Three states, not two. "We have not asked the driver" is reported as
    // null, because false would read as "the radio cannot do it" and that is
    // a claim nothing in this build has earned.
    d.set("accessPoint", c.driver_ap_known ? Json::boolean(c.driver_ap) : Json::null());
    d.set("accessPointKnown", Json::boolean(c.driver_ap_known));
    d.set("scan", Json::boolean(c.driver_scan));
    d.set("concurrentStaAp", Json::boolean(c.driver_concurrent_sta_ap));
    j.set("driverSupports", d);

    Json t = Json::object();
    t.set("wpaSupplicant", Json::boolean(c.wpa_supplicant_available));
    t.set("hostapd", Json::boolean(c.hostapd_available));
    t.set("dhcpServer", Json::boolean(c.dhcp_server_available));
    j.set("tooling", t);

    // What the UI should actually offer: both halves have to agree.
    //
    // For the access point there are two different questions and the document
    // carries both, because answering only one of them forces a lie. A board
    // whose driver we have not asked is ATTEMPTABLE but not VERIFIED -- the
    // control is offered, labelled as unverified, and a failure then comes
    // back as a real error instead of contradicting a promise made here.
    Json u = Json::object();
    u.set("station", Json::boolean(c.station_usable()));
    u.set("accessPoint", Json::boolean(c.ap_attemptable()));
    u.set("accessPointVerified", Json::boolean(c.ap_verified()));
    u.set("scan", Json::boolean(c.scan_usable()));
    j.set("usable", u);

    j.set("apUnavailableReason", Json::string(c.ap_unavailable_reason));
    return j;
}

Json wifi_scan_json(const std::vector<net::WifiNetwork>& networks)
{
    Json a = Json::array();
    for (const auto& n : networks) {
        Json o = Json::object();
        o.set("ssid", Json::string(n.ssid));
        o.set("bssid", Json::string(n.bssid));
        o.set("channel", Json::integer(n.channel));
        o.set("rssiDbm", Json::integer(n.rssi_dbm));
        o.set("security", Json::string(net::wifi_security_name(n.security)));
        a.push(o);
    }
    return a;
}

Json wifi_status_json(const net::WifiCapabilities& caps, net::WifiMode mode,
                      net::LinkState state, const net::WifiNetwork* connected)
{
    Json j = Json::object();
    j.set("mode", Json::string(net::wifi_mode_name(mode)));
    j.set("state", Json::string(net::link_state_name(state)));
    j.set("capabilities", wifi_capabilities_json(caps));
    if (connected) {
        Json c = Json::object();
        c.set("ssid", Json::string(connected->ssid));
        c.set("bssid", Json::string(connected->bssid));
        c.set("channel", Json::integer(connected->channel));
        c.set("rssiDbm", Json::integer(connected->rssi_dbm));
        c.set("security", Json::string(net::wifi_security_name(connected->security)));
        j.set("connected", c);
    }
    // No passphrase, ever. Not even a masked one: a length is information.
    return j;
}

bool wifi_station_from_json(const Json& body, net::WifiStationConfig& cfg, std::string& err)
{
    if (!body.is_object()) { err = "body must be an object"; return false; }
    net::WifiStationConfig next;
    if (!reject_unknown(body, {"ssid", "passphrase", "dhcp", "ip", "netmask", "gateway", "dns"}, nullptr, err)) return false;

    if (!get_string(body, "ssid", next.ssid, err, 32)) return false;
    if (next.ssid.empty()) { err = "ssid is required"; return false; }
    if (!get_string(body, "passphrase", next.passphrase, err, 64)) return false;
    // Not required: an open network legitimately has none.
    if (!check_wifi_secret(next.passphrase, false, err)) return false;

    next.dhcp = true;
    if (!get_bool(body, "dhcp", next.dhcp, err)) return false;
    if (!get_string(body, "ip", next.static_ip, err, 45)) return false;
    if (!get_string(body, "netmask", next.netmask, err, 45)) return false;
    if (!get_string(body, "gateway", next.gateway, err, 45)) return false;
    if (!get_string(body, "dns", next.dns, err, 45)) return false;

    if (!next.dhcp && next.static_ip.empty()) {
        err = "a static configuration needs an ip";
        return false;
    }
    if (!check_optional_ipv4(next.static_ip, "ip", err)) return false;
    if (!check_optional_ipv4(next.netmask, "netmask", err)) return false;
    if (!check_optional_ipv4(next.gateway, "gateway", err)) return false;
    if (!check_optional_ipv4(next.dns, "dns", err)) return false;

    cfg = next;
    return true;
}

bool wifi_ap_from_json(const Json& body, net::WifiApConfig& cfg, std::string& err)
{
    if (!body.is_object()) { err = "body must be an object"; return false; }
    net::WifiApConfig next;
    if (!reject_unknown(body, {"ssid", "passphrase", "security", "channel", "ip", "dhcpStart", "dhcpEnd", "dhcpServer"}, nullptr, err)) return false;

    if (!get_string(body, "ssid", next.ssid, err, 32)) return false;
    if (next.ssid.empty()) { err = "ssid is required"; return false; }
    if (!get_string(body, "passphrase", next.passphrase, err, 64)) return false;

    std::string sec;
    if (!get_string(body, "security", sec, err, 16)) return false;
    if (!sec.empty() && !net::wifi_security_parse(sec, next.security)) {
        err = "security must be open, wpa2, wpa3, wpa2-wpa3 or wep";
        return false;
    }
    // An open AP is a decision, not an accident; a secured one needs a real key.
    if (!check_wifi_secret(next.passphrase, next.security != net::WifiSecurity::Open, err)) return false;

    if (!get_int(body, "channel", next.channel, 0, 196, err)) return false;
    if (!get_string(body, "ip", next.ipv4, err, 45)) return false;
    if (!get_string(body, "dhcpStart", next.dhcp_start, err, 45)) return false;
    if (!get_string(body, "dhcpEnd", next.dhcp_end, err, 45)) return false;
    if (!get_bool(body, "dhcpServer", next.dhcp_server, err)) return false;

    // The AP's own address and its pool go straight into a DHCP server config;
    // a malformed one there is a service that silently does not start.
    if (!check_optional_ipv4(next.ipv4, "ip", err)) return false;
    if (!check_optional_ipv4(next.dhcp_start, "dhcpStart", err)) return false;
    if (!check_optional_ipv4(next.dhcp_end, "dhcpEnd", err)) return false;

    cfg = next;
    return true;
}

// -------------------------------------------------------------- Mobilfunk

namespace {

// Ein Maybe wird null, wenn es leer ist. Das ist der ganze Unterschied
// zwischen "kein Messwert" und "Messwert 0", und in einer Anzeige ist er
// nicht mehr zu erkennen, wenn er hier verlorengeht.
Json maybe_int(const cellular::MaybeInt& v)
{
    return v.has ? Json::integer(v.value) : Json::null();
}

Json str_or_null(const std::string& s)
{
    return s.empty() ? Json::null() : Json::string(s);
}

} // namespace

Json cellular_status_json(const cellular::CellularStatus& s)
{
    Json j = Json::object();
    j.set("present", Json::boolean(s.present));
    j.set("responsive", Json::boolean(s.responsive));

    Json id = Json::object();
    id.set("manufacturer", str_or_null(s.identity.manufacturer));
    id.set("model", str_or_null(s.identity.model));
    id.set("firmware", str_or_null(s.identity.firmware));
    id.set("imei", str_or_null(s.imei));
    j.set("modem", id);

    Json sim = Json::object();
    sim.set("state", Json::string(cellular::sim_state_name(s.sim)));
    sim.set("detail", str_or_null(s.sim_detail));
    sim.set("iccid", str_or_null(s.iccid));
    sim.set("imsi", str_or_null(s.imsi));
    j.set("sim", sim);

    Json net = Json::object();
    net.set("registration", Json::string(cellular::reg_state_name(s.registration)));
    net.set("registered", Json::boolean(cellular::reg_is_registered(s.registration)));
    net.set("roaming", Json::boolean(s.registration == cellular::RegState::RegisteredRoaming));
    net.set("operatorName", str_or_null(s.operator_name));
    net.set("operatorCode", str_or_null(s.operator_code));
    net.set("rat", str_or_null(s.rat));
    j.set("network", net);

    Json rf = Json::object();
    rf.set("band", maybe_int(s.cell.band));
    rf.set("bandMhz", maybe_int(s.cell.band_mhz));
    rf.set("earfcn", maybe_int(s.cell.earfcn));
    rf.set("pci", maybe_int(s.cell.pci));
    rf.set("cellId", str_or_null(s.cell.cell_id));
    rf.set("tac", str_or_null(s.cell.tac));
    rf.set("rsrpDbm", maybe_int(s.cell.rsrp));
    rf.set("rsrqDb", maybe_int(s.cell.rsrq));
    // QENG misst genauer als CSQ. Fehlt es, tritt der aus CSQ abgeleitete Wert
    // ein -- und fehlt auch der, bleibt es null statt 0.
    rf.set("rssiDbm", s.cell.rssi.has ? Json::integer(s.cell.rssi.value)
                                      : maybe_int(s.signal.rssi_dbm));
    rf.set("sinrDb", maybe_int(s.cell.sinr));
    rf.set("csq", maybe_int(s.signal.csq));
    j.set("radio", rf);

    Json pdp = Json::object();
    pdp.set("ipv4", str_or_null(s.pdp.ipv4));
    pdp.set("ipv6", str_or_null(s.pdp.ipv6));
    j.set("pdp", pdp);

    j.set("lastError", str_or_null(s.last_error));
    j.set("lastUpdateMs", Json::integer((double)s.last_update_ms));
    return j;
}

Json cellular_config_json(const cellular::CellularConfig& c)
{
    Json j = Json::object();
    j.set("enabled", Json::boolean(c.enabled));
    j.set("apn", Json::string(c.apn));
    j.set("pdpType", Json::string(cellular::pdp_type_name(c.pdp)));
    j.set("authMode", Json::string(cellular::auth_mode_name(c.auth)));
    j.set("username", Json::string(c.username));
    j.set("autoConnect", Json::boolean(c.auto_connect));
    j.set("nicMode", Json::boolean(c.nic_mode));
    // Welcher Datenlink gewaehlt IST -- was gerade laeuft, steht in dataLink.state
    // des Statusdokuments. Die beiden fallen zwischen Umstellen und Neustart
    // auseinander, und das ist der Punkt.
    j.set("dataLink", Json::string(c.data_link));
    j.set("dataLinkAppliesAt", Json::string("reboot"));
    j.set("dial", Json::string(c.dial));
    // Weder Passwort noch PIN. Nur ob eines hinterlegt ist -- das braucht die
    // Oberflaeche, um "gespeichert" von "leer" zu unterscheiden.
    j.set("passwordSet", Json::boolean(!c.password.empty()));
    j.set("simPinSet", Json::boolean(!c.sim_pin.empty()));
    return j;
}

Json cellular_network_json(const cellular::CellularStatus& s,
                           const cellular::CellularConfig& c,
                           const cellular::CellularLinkState& link,
                           net::LinkState uplink_state,
                           bool internet)
{
    // Die Modemhaelfte kommt unveraendert aus cellular_status_json. Sie hier
    // noch einmal zusammenzubauen hiesse, dass zwei Dokumente dasselbe Modem
    // verschieden beschreiben koennen, sobald jemand nur eines davon pflegt.
    Json j = cellular_status_json(s);

    j.set("enabled", Json::boolean(c.enabled));
    // "available" ist die HARDWARE-Frage und nicht die Zustimmungsfrage: ein
    // Modem steckt oder es steckt nicht, unabhaengig davon, ob jemand den
    // Haken gesetzt hat. Beides in ein Feld zu falten hiesse, dass eine
    // Oberflaeche "kein Modem" anzeigt, wo "eingeschaltet werden muesste"
    // richtig waere.
    j.set("available", Json::boolean(s.present));
    j.set("state", Json::string(net::link_state_name(uplink_state)));
    j.set("internet", Json::boolean(internet));

    Json dl = Json::object();
    // Die Art des Datenlinks steht ausdruecklich drin, und zwar ZWEIMAL:
    //
    //   kind      was gerade laeuft
    //   selected  was gewaehlt ist
    //
    // Die beiden fallen zwischen dem Umstellen und dem Neustart auseinander --
    // dieselbe Lage wie bei usb.mode, und aus demselben Grund abgebildet: eine
    // Oberflaeche, die nur eines von beiden kennt, sagt entweder "laeuft
    // schon" oder "ist aus", und beides waere falsch.
    //
    // Frueher stand hier fest "ecm". Das war richtig, solange es nur einen
    // Datenlink gab, und waere ab AP-M7 eine Behauptung: eine PPP-Verbindung
    // haette sich als ECM ausgegeben.
    dl.set("kind", Json::string(cellular::data_link_kind_name(link.kind)));
    dl.set("selected", Json::string(c.data_link));
    dl.set("rebootRequired",
           Json::boolean(c.data_link != cellular::data_link_kind_name(link.kind)));
    dl.set("state", Json::string(cellular::data_link_state_name(link.state)));
    // nicMode gilt nur fuer ECM. Bei PPP kommt die Adresse aus der
    // IPCP-Aushandlung, und "Routing-Modus" waere dort eine Angabe ueber
    // etwas, das es nicht gibt.
    dl.set("nicMode", link.kind == cellular::DataLinkKind::Ecm
                          ? Json::boolean(link.nic_mode) : Json::null());
    dl.set("attempts", Json::integer(link.attempts));
    dl.set("detail", str_or_null(link.detail));
    j.set("dataLink", dl);

    j.set("interface", str_or_null(link.interface_name));

    Json addr = Json::object();
    addr.set("ipv4", str_or_null(link.address.ipv4));
    addr.set("netmask", str_or_null(link.address.netmask));
    addr.set("gateway", str_or_null(link.address.gateway));
    Json dns = Json::array();
    if (!link.address.dns1.empty()) dns.push(Json::string(link.address.dns1));
    if (!link.address.dns2.empty()) dns.push(Json::string(link.address.dns2));
    addr.set("dns", dns);
    addr.set("mtu", link.address.mtu > 0 ? Json::integer(link.address.mtu) : Json::null());
    j.set("address", addr);

    // Ohne PIN und ohne Passwort -- cellular_config_json gibt beide nicht
    // heraus, sondern nur, ob eines hinterlegt ist.
    j.set("config", cellular_config_json(c));
    return j;
}

Json cellular_presets_json()
{
    Json a = Json::array();
    for (const cellular::ApnPreset& p : cellular::apn_presets()) {
        Json o = Json::object();
        o.set("id", Json::string(p.id));
        o.set("label", Json::string(p.label));
        o.set("apn", Json::string(p.apn));
        o.set("pdpType", Json::string(cellular::pdp_type_name(p.pdp)));
        o.set("authMode", Json::string(cellular::auth_mode_name(p.auth)));
        o.set("username", Json::string(p.user));
        o.set("password", Json::string(p.pass));
        o.set("note", Json::string(p.note));
        a.push(o);
    }
    return a;
}

bool cellular_config_from_json(const Json& body, cellular::CellularConfig& cfg, std::string& err)
{
    if (!body.is_object()) { err = "body must be an object"; return false; }
    // `enabled` ist hier KEIN Feld mehr, und die Ablehnung ist ausdruecklich.
    //
    // Ob Mobilfunk laeuft, entscheidet usb.mode -- es gibt einen USB-Port, und
    // zwei Schalter dafuer waeren zwei Wahrheiten. Ein stilles Ignorieren
    // waere hier die schlechteste Antwort: die Oberflaeche schickt "enabled",
    // bekommt 200, und nichts passiert.
    if (body.get("enabled")) {
        err = "enabled is not set here - the USB port carries one device, "
              "so it is chosen with usb.mode (off, wifi or cellular)";
        return false;
    }
    if (!reject_unknown(body, {"apn", "pdpType", "authMode", "username",
                               "password", "autoConnect", "simPin", "nicMode",
                               "dataLink", "dial"}, nullptr, err)) return false;

    cellular::CellularConfig next = cfg;
    if (!get_bool(body, "autoConnect", next.auto_connect, err)) return false;
    if (!get_bool(body, "nicMode", next.nic_mode, err)) return false;
    if (!get_string(body, "apn", next.apn, err, 100)) return false;
    if (!get_string(body, "dial", next.dial, err, 32)) return false;
    {
        std::string dl = next.data_link;
        if (!get_string(body, "dataLink", dl, err, 8)) return false;
        cellular::DataLinkKind kind;
        if (!cellular::data_link_kind_parse(dl, kind)) {
            err = "dataLink must be ecm or ppp";
            return false;
        }
        next.data_link = dl;
    }
    if (!get_string(body, "username", next.username, err, 64)) return false;

    // Fehlendes Feld heisst "nicht anfassen", leerer String heisst "loeschen".
    // Ohne diesen Unterschied wuerde jede Teilaenderung der Seite die PIN
    // mitloeschen, und beim naechsten Start stuende die SIM gesperrt da.
    if (!get_string(body, "password", next.password, err, 128)) return false;
    if (!get_string(body, "simPin", next.sim_pin, err, 8)) return false;
    if (!next.sim_pin.empty()) {
        if (next.sim_pin.size() < 4) { err = "a SIM PIN is 4 to 8 digits"; return false; }
        for (char c : next.sim_pin)
            if (c < '0' || c > '9') { err = "a SIM PIN is digits only"; return false; }
    }

    std::string s;
    if (!get_string(body, "pdpType", s, err, 16)) return false;
    if (!s.empty() && !cellular::pdp_type_parse(s, next.pdp)) {
        err = "pdpType must be IP or IPV4V6"; return false;
    }
    s.clear();
    if (!get_string(body, "authMode", s, err, 16)) return false;
    if (!s.empty() && !cellular::auth_mode_parse(s, next.auth)) {
        err = "authMode must be none, pap or chap"; return false;
    }
    if (next.auth != cellular::AuthMode::None && next.username.empty()) {
        err = "PAP or CHAP needs a username"; return false;
    }

    // ZULETZT, ueber das Ergebnis und nicht ueber das Fragment: ein PATCH kann
    // ein Feld unberuehrt lassen, und geprueft werden muss, was am Ende in der
    // Konfigurationsdatei steht.
    if (!config_safe(next.apn, "apn", err)) return false;
    if (!config_safe(next.dial, "dial", err)) return false;
    if (!config_safe(next.username, "username", err)) return false;
    if (!config_safe(next.password, "password", err)) return false;

    cfg = next;
    return true;
}

void cellular_config_to_settings(const cellular::CellularConfig& cfg,
                                 std::vector<std::pair<std::string, std::string>>& out)
{
    // KEIN cellular.enabled mehr. Ob Mobilfunk laeuft, sagt usb.mode -- es gibt
    // einen Port, und zwei Schluessel dafuer waeren zwei Wahrheiten, die
    // auseinanderlaufen koennen. Das Feld im Struct bleibt, es wird jetzt
    // abgeleitet.
    out.emplace_back("cellular.apn", cfg.apn);
    out.emplace_back("cellular.pdp_type", cellular::pdp_type_name(cfg.pdp));
    out.emplace_back("cellular.auth_mode", cellular::auth_mode_name(cfg.auth));
    out.emplace_back("cellular.username", cfg.username);
    out.emplace_back("cellular.password", cfg.password);
    out.emplace_back("cellular.auto_connect", cfg.auto_connect ? "true" : "false");
    out.emplace_back("cellular.nic_mode", cfg.nic_mode ? "true" : "false");
    out.emplace_back("cellular.data_link", cfg.data_link);
    out.emplace_back("cellular.dial", cfg.dial);
    out.emplace_back("cellular.sim_pin", cfg.sim_pin);
}

bool cellular_config_from_settings(const std::vector<std::pair<std::string, std::string>>& in,
                                   cellular::CellularConfig& cfg, std::string& err)
{
    cellular::CellularConfig next = cfg;
    for (const auto& kv : in) {
        const std::string& k = kv.first;
        const std::string& v = kv.second;
        // cellular.enabled wird bewusst NICHT gelesen. Eine Datei aus der Zeit
        // davor kann den Schluessel noch tragen; er wird ignoriert, weil
        // usb.mode entscheidet. Ihn zu lesen hiesse, dass ein Rest aus einer
        // alten Installation den USB-Modus ueberstimmt.
        if      (k == "cellular.auto_connect") next.auto_connect = (v == "true" || v == "1");
        else if (k == "cellular.nic_mode")     next.nic_mode = (v == "true" || v == "1");
        else if (k == "cellular.apn")          next.apn = v;
        else if (k == "cellular.username")     next.username = v;
        else if (k == "cellular.password")     next.password = v;
        else if (k == "cellular.sim_pin")      next.sim_pin = v;
        else if (k == "cellular.dial")         next.dial = v;
        else if (k == "cellular.data_link") {
            cellular::DataLinkKind kind;
            if (!cellular::data_link_kind_parse(v, kind)) {
                // Abgelehnt, nicht auf ecm zurueckgefallen. Ein stilles
                // Zurueckfallen sieht aus wie "die Einstellung hat nicht
                // gegriffen" -- und der Bootpfad laedt dann andere Module, als
                // in der Datei steht.
                err = "cellular.data_link is not ecm or ppp: " + v; return false;
            }
            next.data_link = v;
        }
        else if (k == "cellular.pdp_type") {
            if (!cellular::pdp_type_parse(v, next.pdp)) {
                err = "cellular.pdp_type is not IP or IPV4V6"; return false;
            }
        } else if (k == "cellular.auth_mode") {
            if (!cellular::auth_mode_parse(v, next.auth)) {
                err = "cellular.auth_mode is not none, pap or chap"; return false;
            }
        }
    }
    cfg = next;
    return true;
}

}} // namespace machino::api
