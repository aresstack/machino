// The /api/v1/usb and /api/v1/network routing table.
//
// Every route, every status code and every refusal, without a socket. Three
// properties matter more than the individual assertions and are checked
// explicitly:
//
//   * a passphrase that goes in never comes back out
//   * a feature that is not wired answers 404 rather than crashing or lying
//   * a change that can lock the user out is staged, never applied outright
#include "app/api/net_api.hpp"
#include <cstdio>
#include <map>
#include <string>

using namespace machino;
using namespace machino::api;

extern int g_fail_ext, g_pass_ext;
#define TCHECK(cond) do { if (cond) { ++g_pass_ext; } else { ++g_fail_ext; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

namespace {

// ------------------------------------------------------------------ fakes

struct FakeUsbBackend : IUsbHostBackend {
    UsbCapabilities caps;
    bool host = true;
    bool power = false;
    bool refuse = false;
    std::vector<UsbDevice> device_list;

    UsbCapabilities capabilities() const override { return caps; }
    bool host_active() const override { return host; }
    Result set_power(UsbPowerMode, const std::string&, bool, bool on) override {
        if (refuse) return Result::error();
        power = on;
        return Result::ok();
    }
    bool power_state(bool& on_out) const override { on_out = power; return true; }
    std::vector<UsbDevice> devices() const override { return device_list; }
};

UsbCapabilities switchable_board()
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

struct FakeWifi : net::IWifiAdapter {
    net::WifiCapabilities caps;
    std::vector<net::WifiNetwork> found;
    bool scan_fails = false;
    net::WifiMode  mode_ = net::WifiMode::Station;
    net::LinkState state_ = net::LinkState::Down;
    bool have_station = false;
    net::WifiNetwork station;

    net::WifiCapabilities capabilities() const override { return caps; }
    Result scan(std::vector<net::WifiNetwork>& out) override {
        if (scan_fails) return Result::error();
        out = found;
        return Result::ok();
    }
    Result start_station(const net::WifiStationConfig&) override { return Result::ok(); }
    Result start_ap(const net::WifiApConfig&) override { return Result::ok(); }
    Result stop() override { return Result::ok(); }
    net::WifiMode  mode() const override { return mode_; }
    net::LinkState state() const override { return state_; }
    bool station_status(net::WifiNetwork& out) const override {
        if (!have_station) return false;
        out = station;
        return true;
    }
};

net::WifiCapabilities full_radio()
{
    net::WifiCapabilities c;
    c.present = true;
    c.driver_station = c.driver_scan = true;
    c.driver_ap_known = true; c.driver_ap = true;
    c.wpa_supplicant_available = c.hostapd_available = c.dhcp_server_available = true;
    c.ifname = "wlan0";
    c.driver = "aic8800";
    return c;
}

struct FakeUplink : net::INetworkUplink {
    net::UplinkType t; std::string ident;
    net::LinkState st = net::LinkState::Connected;
    bool inet = true;
    FakeUplink(net::UplinkType tt, std::string i) : t(tt), ident(std::move(i)) {}
    std::string id() const override { return ident; }
    net::UplinkType type() const override { return t; }
    net::LinkState state() const override { return st; }
    net::NetworkInfo info() const override { net::NetworkInfo n; n.ifname = ident; return n; }
    net::UplinkMetrics metrics() const override { return net::UplinkMetrics(); }
    Result connect() override { return Result::ok(); }
    Result disconnect() override { return Result::ok(); }
    bool has_internet() const override { return inet; }
};

struct MemStore : IStateStore {
    std::map<std::string, std::string> data;
    bool load(const std::string& k, std::string& out) const override {
        auto it = data.find(k);
        if (it == data.end()) return false;
        out = it->second; return true;
    }
    bool save(const std::string& k, const std::string& v) override { data[k] = v; return true; }
    void clear(const std::string& k) override { data.erase(k); }
};

// ------------------------------------------------------------------- rig

// Everything wired, so a test only has to say what it wants to be different.
struct Rig {
    FakeUsbBackend     backend;
    usb::UsbHostService usb{backend};
    net::ConnectivityManager conn;
    FakeWifi           wifi;
    MemStore           store;
    std::string        applied;
    int                applies = 0;
    uint32_t           now = 1000;

    FakeUplink eth{net::UplinkType::Ethernet, "eth0"};
    FakeUplink wlan{net::UplinkType::Wifi, "wlan0"};

    net::NetworkTxn txn{store, [this](const std::string& c) {
                            ++applies; applied = c; return Result::ok();
                        }};

    Rig()
    {
        backend.caps = switchable_board();
        wifi.caps = full_radio();
        conn.add(&eth);
        conn.add(&wlan);
        std::string e;
        txn.seed_confirmed("{\"kind\":\"boot\"}", e);
    }

    NetApiService::Deps deps()
    {
        NetApiService::Deps d;
        d.usb  = &usb;
        d.conn = &conn;
        d.wifi = &wifi;
        d.txn  = &txn;
        d.now_ms = [this] { return now; };
        d.confirm_window_ms = 60000;
        return d;
    }
};

struct Call {
    bool     routed = false;
    Response r;
};

Call call(NetApiService& api, const char* m, const std::string& path, const std::string& body = "")
{
    Call c;
    c.routed = api.handle(m, path, body, c.r);
    return c;
}

std::string dumped(const Response& r) { return r.body.dump(); }
bool contains(const std::string& hay, const std::string& needle)
{
    return hay.find(needle) != std::string::npos;
}

// ------------------------------------------------------------------ tests

void test_paths_outside_our_prefixes_are_not_claimed()
{
    // The media routes must keep working. handle() returning true for
    // /api/v1/state would take the whole camera API down.
    Rig rig; NetApiService api(rig.deps());
    for (const char* p : {"/api/v1/state", "/api/v1/config", "/", "/api/v1",
                          "/api/v1/usbthing", "/api/v1/networkish"}) {
        Call c = call(api, "GET", p);
        TCHECK(!c.routed);
    }
}

void test_a_typo_under_our_prefix_is_our_404()
{
    // Falling through would hand the client the front-door relay's HTML, which
    // a JSON client cannot read.
    Rig rig; NetApiService api(rig.deps());
    Call c = call(api, "GET", "/api/v1/network/wifi/scannn");
    TCHECK(c.routed && c.r.status == 404);
    Call u = call(api, "GET", "/api/v1/usb/nonsense");
    TCHECK(u.routed && u.r.status == 404);
}

void test_usb_get_reports_status_and_config()
{
    Rig rig; NetApiService api(rig.deps());
    Call c = call(api, "GET", "/api/v1/usb");
    TCHECK(c.routed && c.r.status == 200);
    TCHECK(c.r.body.has("capabilities"));
    TCHECK(c.r.body.has("config"));
}

void test_usb_patch_applies_and_persists()
{
    Rig rig;
    NetApiService::Deps d = rig.deps();
    usb::UsbConfig saved;
    int saves = 0;
    d.save_usb = [&](const usb::UsbConfig& c, std::string&) { saved = c; ++saves; return true; };
    NetApiService api(d);

    Call c = call(api, "PATCH", "/api/v1/usb", "{\"enabled\":true}");
    TCHECK(c.routed && c.r.status == 200);
    TCHECK(saves == 1 && saved.enabled);
    TCHECK(rig.backend.power);           // the port really came up
}

void test_usb_patch_that_cannot_be_persisted_is_an_error_not_a_silent_apply()
{
    // A port that is on now and off after a reboot is the confusing failure.
    Rig rig;
    NetApiService::Deps d = rig.deps();
    d.save_usb = [](const usb::UsbConfig&, std::string& e) { e = "read-only filesystem"; return false; };
    NetApiService api(d);

    Call c = call(api, "PATCH", "/api/v1/usb", "{\"enabled\":true}");
    TCHECK(c.routed && c.r.status == 500);
    TCHECK(!rig.backend.power);
    TCHECK(contains(dumped(c.r), "read-only"));
}

void test_a_usb_apply_that_fails_does_not_leave_the_setting_persisted()
{
    // The config is written before it is applied, so a failing apply has to
    // put the old one back: a 500 that nevertheless changes what the port does
    // at the next boot is the worst of both answers.
    Rig rig;
    rig.backend.refuse = true;
    NetApiService::Deps d = rig.deps();
    usb::UsbConfig saved;
    int saves = 0;
    d.save_usb = [&](const usb::UsbConfig& c, std::string&) { saved = c; ++saves; return true; };
    NetApiService api(d);

    Call c = call(api, "PATCH", "/api/v1/usb", "{\"enabled\":true}");
    TCHECK(c.routed && c.r.status == 500);
    TCHECK(saves == 2);              // written, then put back
    TCHECK(!saved.enabled);          // and what is on disk is the old setting
}

void test_when_the_rollback_of_the_stored_setting_also_fails_it_is_said_so()
{
    Rig rig;
    rig.backend.refuse = true;
    NetApiService::Deps d = rig.deps();
    int saves = 0;
    d.save_usb = [&](const usb::UsbConfig&, std::string& e) {
        if (++saves == 1) return true;
        e = "read-only filesystem";
        return false;
    };
    NetApiService api(d);

    Call c = call(api, "PATCH", "/api/v1/usb", "{\"enabled\":true}");
    TCHECK(c.routed && c.r.status == 500);
    // No reassuring message: the stored configuration really is the bad one.
    TCHECK(contains(dumped(c.r), "could also not be restored"));
}

void test_usb_patch_rejects_an_unknown_field()
{
    Rig rig; NetApiService api(rig.deps());
    Call c = call(api, "PATCH", "/api/v1/usb", "{\"enabeld\":true}");
    TCHECK(c.routed && c.r.status == 422);
    TCHECK(!rig.backend.power);
}

void test_usb_patch_rejects_malformed_json()
{
    Rig rig; NetApiService api(rig.deps());
    Call c = call(api, "PATCH", "/api/v1/usb", "{\"enabled\":");
    TCHECK(c.routed && c.r.status == 400);
    Call arr = call(api, "PATCH", "/api/v1/usb", "[1,2]");
    TCHECK(arr.routed && arr.r.status == 400);
}

void test_wrong_methods_are_405_and_say_what_is_allowed()
{
    Rig rig; NetApiService api(rig.deps());
    Call c = call(api, "DELETE", "/api/v1/usb");
    TCHECK(c.routed && c.r.status == 405 && contains(dumped(c.r), "GET or PATCH"));
    Call s = call(api, "GET", "/api/v1/network/wifi/scan");
    TCHECK(s.routed && s.r.status == 405 && contains(dumped(s.r), "POST"));
}

void test_scan_is_a_post_because_it_is_not_idempotent()
{
    // A GET invites a browser or a proxy to repeat it, and on a single-radio
    // board a repeated scan costs the station connection.
    Rig rig;
    net::WifiNetwork n; n.ssid = "home"; n.channel = 6; n.rssi_dbm = -50;
    n.security = net::WifiSecurity::Wpa2;
    rig.wifi.found.push_back(n);
    NetApiService api(rig.deps());

    Call c = call(api, "POST", "/api/v1/network/wifi/scan");
    TCHECK(c.routed && c.r.status == 200);
    TCHECK(contains(dumped(c.r), "home"));
}

void test_scan_on_a_radio_that_cannot_scan_is_409()
{
    Rig rig;
    rig.wifi.caps.driver_scan = false;
    NetApiService api(rig.deps());
    Call c = call(api, "POST", "/api/v1/network/wifi/scan");
    TCHECK(c.routed && c.r.status == 409);
}

void test_a_failed_scan_is_503_not_an_empty_list()
{
    // An empty list means "nothing on the air"; the UI would tell the user
    // there are no networks when the radio simply did not answer.
    Rig rig;
    rig.wifi.scan_fails = true;
    NetApiService api(rig.deps());
    Call c = call(api, "POST", "/api/v1/network/wifi/scan");
    TCHECK(c.routed && c.r.status == 503);
}

void test_a_wifi_change_is_staged_not_applied_outright()
{
    Rig rig; NetApiService api(rig.deps());
    Call c = call(api, "POST", "/api/v1/network/wifi/station",
                  "{\"ssid\":\"home\",\"passphrase\":\"supersecret1\"}");
    TCHECK(c.routed && c.r.status == 202);
    TCHECK(rig.txn.pending());
    const Json* url = c.r.body.get("confirm_url");
    TCHECK(url && contains(url->as_string(), "/confirm"));
}

void test_the_passphrase_never_comes_back()
{
    Rig rig; NetApiService api(rig.deps());
    Call post = call(api, "POST", "/api/v1/network/wifi/station",
                     "{\"ssid\":\"home\",\"passphrase\":\"supersecret1\"}");
    TCHECK(!contains(dumped(post.r), "supersecret1"));

    rig.wifi.have_station = true;
    rig.wifi.station.ssid = "home";
    Call get = call(api, "GET", "/api/v1/network/wifi");
    TCHECK(!contains(dumped(get.r), "supersecret1"));

    Call net = call(api, "GET", "/api/v1/network");
    TCHECK(!contains(dumped(net.r), "supersecret1"));

    Call ch = call(api, "GET", "/api/v1/network/change");
    TCHECK(!contains(dumped(ch.r), "supersecret1"));
}

void test_a_station_request_without_an_ssid_is_refused()
{
    // "Join anything" is not a sensible default for a camera.
    Rig rig; NetApiService api(rig.deps());
    Call c = call(api, "POST", "/api/v1/network/wifi/station", "{\"passphrase\":\"supersecret1\"}");
    TCHECK(c.routed && c.r.status == 422);
    TCHECK(!rig.txn.pending());
}

void test_confirm_clears_the_pending_change()
{
    Rig rig; NetApiService api(rig.deps());
    Call c = call(api, "POST", "/api/v1/network/wifi/station",
                  "{\"ssid\":\"home\",\"passphrase\":\"supersecret1\"}");
    const std::string token = c.r.body.get("token")->as_string();

    Call ok = call(api, "POST", "/api/v1/network/change/" + token + "/confirm");
    TCHECK(ok.routed && ok.r.status == 200);
    TCHECK(!rig.txn.pending());

    // And the confirmed record now holds the new configuration, so a later
    // rollback restores this one rather than the boot-time state.
    std::string good;
    TCHECK(rig.txn.confirmed_config(good) && contains(good, "wifi-station"));
}

void test_a_wrong_token_does_not_confirm()
{
    Rig rig; NetApiService api(rig.deps());
    call(api, "POST", "/api/v1/network/wifi/station",
         "{\"ssid\":\"home\",\"passphrase\":\"supersecret1\"}");
    Call bad = call(api, "POST", "/api/v1/network/change/999999/confirm");
    TCHECK(bad.routed && bad.r.status == 409);
    TCHECK(rig.txn.pending());
}

void test_a_malformed_confirm_url_is_not_a_confirmation()
{
    // A truncated URL must not be read as "confirm whatever is pending".
    Rig rig; NetApiService api(rig.deps());
    call(api, "POST", "/api/v1/network/wifi/station",
         "{\"ssid\":\"home\",\"passphrase\":\"supersecret1\"}");
    for (const char* p : {"/api/v1/network/change//confirm",
                          "/api/v1/network/change/abc/confirm",
                          "/api/v1/network/change/confirm",
                          "/api/v1/network/change/1/confirmx"}) {
        Call c = call(api, "POST", p);
        TCHECK(c.routed && c.r.status == 404);
    }
    TCHECK(rig.txn.pending());
}

void test_an_unconfirmed_change_rolls_back_on_tick()
{
    Rig rig; NetApiService api(rig.deps());
    call(api, "POST", "/api/v1/network/wifi/station",
         "{\"ssid\":\"home\",\"passphrase\":\"supersecret1\"}");
    TCHECK(rig.txn.pending());

    rig.now += 30000;
    TCHECK(!api.tick());                 // still inside the window
    TCHECK(rig.txn.pending());

    rig.now += 40000;
    TCHECK(api.tick());
    TCHECK(!rig.txn.pending());
    TCHECK(contains(rig.applied, "boot"));   // the known-good one went back on
}

void test_a_second_change_is_refused_while_one_is_pending()
{
    Rig rig; NetApiService api(rig.deps());
    call(api, "POST", "/api/v1/network/wifi/station",
         "{\"ssid\":\"home\",\"passphrase\":\"supersecret1\"}");
    Call second = call(api, "POST", "/api/v1/network/wifi/station",
                       "{\"ssid\":\"other\",\"passphrase\":\"supersecret2\"}");
    TCHECK(second.routed && second.r.status == 409);
}

void test_change_status_reports_the_countdown()
{
    Rig rig; NetApiService api(rig.deps());
    Call idle = call(api, "GET", "/api/v1/network/change");
    TCHECK(idle.routed && idle.r.body.get("pending") && !idle.r.body.get("pending")->as_bool());

    call(api, "POST", "/api/v1/network/wifi/station",
         "{\"ssid\":\"home\",\"passphrase\":\"supersecret1\"}");
    rig.now += 10000;
    Call live = call(api, "GET", "/api/v1/network/change");
    TCHECK(live.r.body.get("pending")->as_bool());
    TCHECK(live.r.body.get("remaining_ms")->as_int() == 50000);
}

void test_ap_mode_without_hostapd_says_which_half_is_missing()
{
    Rig rig;
    rig.wifi.caps.hostapd_available = false;
    rig.wifi.caps.ap_unavailable_reason = "hostapd is not in this image";
    NetApiService api(rig.deps());

    Call c = call(api, "POST", "/api/v1/network/wifi/ap", "{\"ssid\":\"cam\",\"passphrase\":\"supersecret1\"}");
    TCHECK(c.routed && c.r.status == 409);
    TCHECK(contains(dumped(c.r), "hostapd"));
}

void test_ap_mode_without_a_dhcp_server_is_refused()
{
    // Clients would associate and then sit there with no address, which looks
    // exactly like a broken camera.
    Rig rig;
    rig.wifi.caps.dhcp_server_available = false;
    NetApiService api(rig.deps());
    Call c = call(api, "POST", "/api/v1/network/wifi/ap",
                  "{\"ssid\":\"cam\",\"passphrase\":\"supersecret1\",\"dhcpServer\":true}");
    TCHECK(c.routed && c.r.status == 409);
    TCHECK(contains(dumped(c.r), "DHCP"));
}

void test_policy_patch_takes_effect_at_once()
{
    Rig rig;
    NetApiService::Deps d = rig.deps();
    int saves = 0;
    d.save_policy = [&](const net::UplinkPolicy&, std::string&) { ++saves; return true; };
    NetApiService api(d);

    rig.conn.evaluate();
    TCHECK(rig.conn.active_id() == "eth0");

    Call c = call(api, "PATCH", "/api/v1/network/policy", "{\"order\":[\"wifi\",\"ethernet\"]}");
    TCHECK(c.routed && c.r.status == 200);
    TCHECK(saves == 1);
    TCHECK(rig.conn.active_id() == "wlan0");     // not waiting for the next poll
}

void test_policy_patch_that_cannot_be_persisted_changes_nothing()
{
    Rig rig;
    NetApiService::Deps d = rig.deps();
    d.save_policy = [](const net::UplinkPolicy&, std::string& e) { e = "no space left"; return false; };
    NetApiService api(d);
    rig.conn.evaluate();

    Call c = call(api, "PATCH", "/api/v1/network/policy", "{\"order\":[\"wifi\"]}");
    TCHECK(c.routed && c.r.status == 500);
    TCHECK(rig.conn.active_id() == "eth0");
}

void test_unwired_features_are_404_not_a_crash()
{
    NetApiService::Deps d;               // nothing wired at all
    d.now_ms = [] { return 0u; };
    NetApiService api(d);

    for (const char* p : {"/api/v1/usb", "/api/v1/usb/devices", "/api/v1/network",
                          "/api/v1/network/wifi"}) {
        Call c = call(api, "GET", p);
        TCHECK(c.routed && c.r.status == 404);
    }
    Call scan = call(api, "POST", "/api/v1/network/wifi/scan");
    TCHECK(scan.routed && scan.r.status == 404);
    Call conf = call(api, "POST", "/api/v1/network/change/1/confirm");
    TCHECK(conf.routed && conf.r.status == 404);
    TCHECK(!api.tick());
}

void test_a_staged_change_without_a_known_good_baseline_is_refused()
{
    // Applying a candidate with nothing to fall back to is a one-way door.
    MemStore store;
    net::NetworkTxn txn(store, [](const std::string&) { return Result::ok(); });
    FakeWifi wifi; wifi.caps = full_radio();

    NetApiService::Deps d;
    d.wifi = &wifi;
    d.txn = &txn;
    d.now_ms = [] { return 0u; };
    NetApiService api(d);

    Call c = call(api, "POST", "/api/v1/network/wifi/station",
                  "{\"ssid\":\"home\",\"passphrase\":\"supersecret1\"}");
    TCHECK(c.routed && c.r.status == 409);
    TCHECK(!txn.pending());
}

} // namespace

void run_net_api_tests()
{
    test_paths_outside_our_prefixes_are_not_claimed();
    test_a_typo_under_our_prefix_is_our_404();
    test_usb_get_reports_status_and_config();
    test_usb_patch_applies_and_persists();
    test_usb_patch_that_cannot_be_persisted_is_an_error_not_a_silent_apply();
    test_a_usb_apply_that_fails_does_not_leave_the_setting_persisted();
    test_when_the_rollback_of_the_stored_setting_also_fails_it_is_said_so();
    test_usb_patch_rejects_an_unknown_field();
    test_usb_patch_rejects_malformed_json();
    test_wrong_methods_are_405_and_say_what_is_allowed();
    test_scan_is_a_post_because_it_is_not_idempotent();
    test_scan_on_a_radio_that_cannot_scan_is_409();
    test_a_failed_scan_is_503_not_an_empty_list();
    test_a_wifi_change_is_staged_not_applied_outright();
    test_the_passphrase_never_comes_back();
    test_a_station_request_without_an_ssid_is_refused();
    test_confirm_clears_the_pending_change();
    test_a_wrong_token_does_not_confirm();
    test_a_malformed_confirm_url_is_not_a_confirmation();
    test_an_unconfirmed_change_rolls_back_on_tick();
    test_a_second_change_is_refused_while_one_is_pending();
    test_change_status_reports_the_countdown();
    test_ap_mode_without_hostapd_says_which_half_is_missing();
    test_ap_mode_without_a_dhcp_server_is_refused();
    test_policy_patch_takes_effect_at_once();
    test_policy_patch_that_cannot_be_persisted_changes_nothing();
    test_unwired_features_are_404_not_a_crash();
    test_a_staged_change_without_a_known_good_baseline_is_refused();
}
