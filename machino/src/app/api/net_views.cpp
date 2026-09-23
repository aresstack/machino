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
    if (!reject_unknown(body, {"enabled", "power"}, nullptr, err)) return false;
    if (!get_bool(body, "enabled", next.enabled, err)) return false;

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
}

bool usb_config_from_settings(const std::vector<std::pair<std::string, std::string>>& in,
                              usb::UsbConfig& cfg, std::string& err)
{
    usb::UsbConfig next = cfg;
    for (const auto& kv : in) {
        const std::string& k = kv.first;
        const std::string& v = kv.second;
        if      (k == "usb.enabled")               next.enabled = (v == "true" || v == "1");
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

}} // namespace machino::api
