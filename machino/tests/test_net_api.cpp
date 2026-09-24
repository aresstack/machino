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
#include "scripted_at_transport.hpp"
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

// A modem that is not there. Enough for the API surface: what is being pinned
// here is the document shape, the staging and the secrets -- not the state
// machine, which has its own file.
struct AbsentEcmBackend : IEcmBackend {
    bool find_interface(EcmInterface&) override { return false; }
    bool set_up(const std::string&, bool) override { return false; }
    bool dhcp_start(const std::string&) override { return false; }
    bool dhcp_stop(const std::string&) override { return true; }
    bool read_address(const std::string&, EcmAddress&) override { return false; }
    bool set_address(const std::string&, const EcmAddress&) override { return false; }
    void teardown(const std::string&) override {}
};

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

    machino::test::ScriptedAtTransport at;
    AbsentEcmBackend          ecm;
    cellular::CellularService cell_svc{at};
    cellular::EcmLink         cell_link{at, ecm};
    net::CellularUplink       cell{cell_svc, cell_link};

    // The stand-in for the configuration store. Written ONLY on confirm, which
    // is the invariant main.cpp depends on: a staged change deviates from it
    // live, and every apply -- the rollback's included -- resets to it first.
    cellular::CellularConfig stored;

    net::NetworkTxn txn{store, [this](const std::string& c) {
                            ++applies; applied = c;
                            // Mirrors the apply lambda in main.cpp, including
                            // the reset. Writing a different one here would
                            // test a runtime that does not exist.
                            cell.set_config(stored);
                            Json j; std::string e;
                            if (!Json::parse(c, j, e)) return Result::error();
                            const Json* kind = j.get("kind");
                            if (!kind || !kind->is_string()) return Result::error();
                            if (kind->as_string() == "cellular") {
                                const Json* conf = j.get("config");
                                cellular::CellularConfig cc = cell.config();
                                if (!conf || !cellular_config_from_json(*conf, cc, e))
                                    return Result::error();
                                cell.set_config(cc);
                            }
                            return Result::ok();
                        }};

    Rig()
    {
        backend.caps = switchable_board();
        wifi.caps = full_radio();
        conn.add(&eth);
        conn.add(&wlan);
        conn.add(&cell);
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
        d.cellular = &cell;
        d.now_ms = [this] { return now; };
        d.confirm_window_ms = 60000;
        d.on_confirmed = [this](const std::string& candidate) {
            confirmed.push_back(candidate);
            Json j; std::string e;
            if (!Json::parse(candidate, j, e)) return;
            const Json* kind = j.get("kind");
            if (kind && kind->is_string() && kind->as_string() == "cellular")
                stored = cell.config();
        };
        return d;
    }

    std::vector<std::string> confirmed;
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

// ------------------------------------------------------------- Mobilfunk

void test_the_cellular_document_never_carries_the_pin_or_the_password()
{
    Rig r;
    cellular::CellularConfig c;
    c.enabled = true;
    c.apn = "internet.t-d1.de";
    c.username = "user";
    c.password = "hunter2-apn-password";
    c.sim_pin = "4711";
    r.cell.set_config(c);
    NetApiService api(r.deps());

    for (const char* path : {"/api/v1/network/cellular", "/api/v1/network"}) {
        const Call g = call(api, "GET", path);
        TCHECK(g.routed && g.r.status == 200);
        const std::string doc = dumped(g.r);
        TCHECK(!contains(doc, "hunter2-apn-password"));
        TCHECK(!contains(doc, "4711"));
        // What a UI actually needs: whether something is stored.
        TCHECK(contains(doc, "\"simPinSet\":true"));
        TCHECK(contains(doc, "\"passwordSet\":true"));
    }
}

void test_the_cellular_document_separates_the_modem_from_the_link()
{
    // A modem can be registered beautifully and still move no traffic. A page
    // that only shows one of the two halves cannot name that case.
    Rig r;
    NetApiService api(r.deps());
    const Call g = call(api, "GET", "/api/v1/network/cellular");
    TCHECK(g.routed && g.r.status == 200);
    const std::string doc = dumped(g.r);
    TCHECK(contains(doc, "\"dataLink\""));
    TCHECK(contains(doc, "\"kind\":\"ecm\""));      // named, because AP-M7 brings PPP
    TCHECK(contains(doc, "\"modem\""));
    TCHECK(contains(doc, "\"sim\""));
    TCHECK(contains(doc, "\"interface\""));
    TCHECK(contains(doc, "\"address\""));
    TCHECK(contains(doc, "\"internet\":false"));
    // No modem: absent, not failed. Nothing is broken, there is just nothing
    // there.
    TCHECK(contains(doc, "\"state\":\"absent\""));
}

void test_the_network_document_carries_cellular_as_one_uplink_of_three()
{
    Rig r;
    NetApiService api(r.deps());
    const std::string doc = dumped(call(api, "GET", "/api/v1/network").r);
    TCHECK(contains(doc, "\"id\":\"eth0\""));
    TCHECK(contains(doc, "\"id\":\"wlan0\""));
    // The logical name, not usb0 -- that is what a preference list has to be
    // able to say and what survives an interface rename.
    TCHECK(contains(doc, "\"id\":\"cellular\""));
    TCHECK(contains(doc, "\"type\":\"cellular\""));
}

void test_a_cellular_change_is_staged_and_not_applied_outright()
{
    Rig r;
    NetApiService api(r.deps());
    const Call p = call(api, "PATCH", "/api/v1/network/cellular",
                        "{\"enabled\":true,\"apn\":\"internet.t-d1.de\"}");
    TCHECK(p.routed && p.r.status == 202);
    TCHECK(contains(dumped(p.r), "\"pending\":true"));
    // Live already -- that is what staging means. Not yet permanent.
    TCHECK(r.cell.config().apn == "internet.t-d1.de");
    TCHECK(r.stored.apn.empty());
    TCHECK(r.confirmed.empty());
}

void test_a_confirmed_cellular_change_becomes_permanent()
{
    Rig r;
    NetApiService api(r.deps());
    const Call p = call(api, "PATCH", "/api/v1/network/cellular",
                        "{\"enabled\":true,\"apn\":\"netpublic\"}");
    const std::string token = p.r.body.get("token")->as_string();

    const Call c = call(api, "POST", "/api/v1/network/change/" + token + "/confirm");
    TCHECK(c.routed && c.r.status == 200);
    TCHECK(r.confirmed.size() == 1);
    TCHECK(r.stored.apn == "netpublic");
    TCHECK(r.stored.enabled);
}

void test_an_unconfirmed_cellular_change_is_undone()
{
    // The point of staging, and the reason it applies to cellular at all:
    // switching the modem on can move the default route off Ethernet, and
    // whoever administers the camera through that route is then the one who
    // can no longer confirm anything.
    Rig r;
    r.stored.apn = "internet.t-d1.de";
    r.cell.set_config(r.stored);
    NetApiService api(r.deps());

    call(api, "PATCH", "/api/v1/network/cellular", "{\"apn\":\"wrong.example\"}");
    TCHECK(r.cell.config().apn == "wrong.example");

    r.now += 60001;
    TCHECK(api.tick());
    TCHECK(r.cell.config().apn == "internet.t-d1.de");   // really back
    TCHECK(r.stored.apn == "internet.t-d1.de");
    TCHECK(r.confirmed.empty());                          // never became permanent
}

void test_a_rolled_back_change_cannot_be_confirmed_afterwards()
{
    Rig r;
    NetApiService api(r.deps());
    const Call p = call(api, "PATCH", "/api/v1/network/cellular", "{\"apn\":\"wrong.example\"}");
    const std::string token = p.r.body.get("token")->as_string();

    r.now += 60001;
    TCHECK(api.tick());

    const Call c = call(api, "POST", "/api/v1/network/change/" + token + "/confirm");
    TCHECK(c.r.status == 409);
    TCHECK(r.confirmed.empty());
    TCHECK(r.stored.apn.empty());
}

void test_a_partial_cellular_patch_does_not_clear_the_pin()
{
    // A PATCH is partial. A missing simPin means "leave it alone", and getting
    // that backwards would wipe the PIN every time somebody edited the APN.
    Rig r;
    cellular::CellularConfig c;
    c.sim_pin = "4711";
    c.apn = "old.example";
    r.stored = c;
    r.cell.set_config(c);
    NetApiService api(r.deps());

    call(api, "PATCH", "/api/v1/network/cellular", "{\"apn\":\"new.example\"}");
    TCHECK(r.cell.config().apn == "new.example");
    TCHECK(r.cell.config().sim_pin == "4711");
}

void test_an_unknown_cellular_field_changes_nothing()
{
    Rig r;
    NetApiService api(r.deps());
    const Call p = call(api, "PATCH", "/api/v1/network/cellular",
                        "{\"apn\":\"x\",\"turbo\":true}");
    TCHECK(p.r.status == 422);
    TCHECK(r.cell.config().apn.empty());
    TCHECK(!r.txn.pending());
}

void test_cellular_presets_are_offered_with_their_reason()
{
    Rig r;
    NetApiService api(r.deps());
    const Call g = call(api, "GET", "/api/v1/network/cellular/presets");
    TCHECK(g.routed && g.r.status == 200);
    const std::string doc = dumped(g.r);
    TCHECK(contains(doc, "internet.t-d1.de"));
    TCHECK(contains(doc, "netpublic"));
    // Suggestions, not automation -- the note says why each one is there.
    TCHECK(contains(doc, "\"note\""));
}

void test_cellular_routes_answer_404_when_no_modem_support_is_wired()
{
    Rig r;
    NetApiService::Deps d = r.deps();
    d.cellular = nullptr;
    NetApiService api(d);
    TCHECK(call(api, "GET", "/api/v1/network/cellular").r.status == 404);
    TCHECK(call(api, "PATCH", "/api/v1/network/cellular", "{}").r.status == 404);
    // And the network document leaves the section out entirely rather than
    // reporting an empty one, so a page can tell "this build has no cellular"
    // from "the modem is quiet".
    TCHECK(!contains(dumped(call(api, "GET", "/api/v1/network").r), "\"dataLink\""));
}

void test_cellular_rejects_the_wrong_method()
{
    Rig r;
    NetApiService api(r.deps());
    TCHECK(call(api, "POST", "/api/v1/network/cellular", "{}").r.status == 405);
    TCHECK(call(api, "DELETE", "/api/v1/network/cellular/presets").r.status == 405);
}

void run_net_api_tests()
{
    test_the_cellular_document_never_carries_the_pin_or_the_password();
    test_the_cellular_document_separates_the_modem_from_the_link();
    test_the_network_document_carries_cellular_as_one_uplink_of_three();
    test_a_cellular_change_is_staged_and_not_applied_outright();
    test_a_confirmed_cellular_change_becomes_permanent();
    test_an_unconfirmed_cellular_change_is_undone();
    test_a_rolled_back_change_cannot_be_confirmed_afterwards();
    test_a_partial_cellular_patch_does_not_clear_the_pin();
    test_an_unknown_cellular_field_changes_nothing();
    test_cellular_presets_are_offered_with_their_reason();
    test_cellular_routes_answer_404_when_no_modem_support_is_wired();
    test_cellular_rejects_the_wrong_method();
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
