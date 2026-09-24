#include "app/api/net_api.hpp"

#include <cstdlib>

namespace machino { namespace api {

namespace {

Response ok(const Json& j) { return Response{200, j}; }

Response not_wired(const std::string& path, const char* what)
{
    // 404, not 501: from the client's point of view the resource genuinely is
    // not there on this build, and a UI that hides a missing section is the
    // behaviour we want. The message says which piece is absent so a support
    // log answers "why is there no WiFi page" without a second round trip.
    return ApiService::fail(404, "unknown_field", path, std::string(what) + " is not available on this build");
}

bool parse_body(const std::string& body, const std::string& path, Json& out, Response& err)
{
    std::string e;
    if (!Json::parse(body, out, e)) {
        err = ApiService::fail(400, "invalid_json", path, e);
        return false;
    }
    if (!out.is_object()) {
        err = ApiService::fail(400, "invalid_value", path, "the body must be a JSON object");
        return false;
    }
    return true;
}

// "/api/v1/network/change/7/confirm" -> "7". Returns false on any other shape,
// including a missing or empty token, so a truncated URL cannot be read as
// "confirm whatever is pending".
bool split_confirm(const std::string& path, std::string& token_out)
{
    const std::string prefix = "/api/v1/network/change/";
    const std::string suffix = "/confirm";
    if (path.size() <= prefix.size() + suffix.size()) return false;
    if (path.compare(0, prefix.size(), prefix) != 0) return false;
    if (path.compare(path.size() - suffix.size(), suffix.size(), suffix) != 0) return false;
    token_out = path.substr(prefix.size(), path.size() - prefix.size() - suffix.size());
    if (token_out.empty()) return false;
    for (char c : token_out) if (c < '0' || c > '9') return false;
    return true;
}

} // namespace

NetApiService::NetApiService(Deps d) : d_(std::move(d)) {}

bool NetApiService::handle(const std::string& method, const std::string& path,
                           const std::string& body, Response& out)
{
    auto wrong_method = [&](const char* allowed) {
        out = ApiService::fail(405, "unknown_field", path,
                               std::string("method not allowed, use ") + allowed);
        return true;
    };

    if (path == "/api/v1/usb") {
        if (method == "GET")   { out = usb_get(); return true; }
        if (method == "PATCH") { out = usb_patch(body); return true; }
        return wrong_method("GET or PATCH");
    }
    if (path == "/api/v1/usb/devices") {
        if (method == "GET") { out = usb_devices(); return true; }
        return wrong_method("GET");
    }
    if (path == "/api/v1/network") {
        if (method == "GET") { out = network_get(); return true; }
        return wrong_method("GET");
    }
    if (path == "/api/v1/network/policy") {
        if (method == "GET")   { out = ok(net_views_policy()); return true; }
        if (method == "PATCH") { out = policy_patch(body); return true; }
        return wrong_method("GET or PATCH");
    }
    if (path == "/api/v1/network/wifi") {
        if (method == "GET") { out = wifi_get(); return true; }
        return wrong_method("GET");
    }
    if (path == "/api/v1/network/wifi/scan") {
        // POST, not GET: a scan takes seconds, disturbs the radio and is not
        // idempotent. Making it a GET invites a browser or a proxy to repeat
        // it, and on a single-radio board that costs the station connection.
        if (method == "POST") { out = wifi_scan(); return true; }
        return wrong_method("POST");
    }
    if (path == "/api/v1/network/wifi/station") {
        if (method == "POST") { out = wifi_station(body); return true; }
        return wrong_method("POST");
    }
    if (path == "/api/v1/network/wifi/ap") {
        if (method == "POST") { out = wifi_ap(body); return true; }
        return wrong_method("POST");
    }
    if (path == "/api/v1/network/cellular") {
        if (method == "GET")   { out = cellular_get(); return true; }
        if (method == "PATCH") { out = cellular_patch(body); return true; }
        return wrong_method("GET or PATCH");
    }
    if (path == "/api/v1/network/cellular/presets") {
        if (method == "GET") { out = cellular_presets(); return true; }
        return wrong_method("GET");
    }
    if (path == "/api/v1/network/change") {
        if (method == "GET") { out = change_get(); return true; }
        return wrong_method("GET");
    }

    std::string token;
    if (split_confirm(path, token)) {
        if (method == "POST") { out = change_confirm(token); return true; }
        return wrong_method("POST");
    }

    // Anything else under our two prefixes is ours to reject, so a typo gets a
    // 404 from here rather than falling through to the static file handler and
    // coming back as an HTML page a JSON client cannot read.
    if (path.compare(0, 12, "/api/v1/usb/") == 0 ||
        path.compare(0, 16, "/api/v1/network/") == 0) {
        out = ApiService::fail(404, "unknown_field", path, "no such resource");
        return true;
    }
    return false;
}

// ------------------------------------------------------------------- USB

Response NetApiService::usb_get() const
{
    if (!d_.usb) return not_wired("/api/v1/usb", "the USB host");
    Json j = usb_status_json(d_.usb->status());
    j.set("config", usb_config_json(d_.usb->config()));
    return ok(j);
}

Response NetApiService::usb_patch(const std::string& body)
{
    const std::string path = "/api/v1/usb";
    if (!d_.usb) return not_wired(path, "the USB host");

    Json j; Response err;
    if (!parse_body(body, path, j, err)) return err;

    // Patch onto the CURRENT configuration, then validate the result as a
    // whole. Validating the fragment would accept "pin without expert" when
    // expert is already set, and reject it when it arrives in the same body.
    usb::UsbConfig cfg = d_.usb->config();
    std::string e;
    if (!usb_config_from_json(j, cfg, e)) return ApiService::fail(422, "invalid_value", path, e);

    usb::UsbResolved resolved;
    if (!usb::UsbHostService::resolve(cfg, d_.usb->capabilities(), resolved, e))
        return ApiService::fail(422, "invalid_value", path, e);

    // Persist BEFORE applying. A port that is on now and off after a reboot is
    // the confusing failure; a change that did not take is not.
    const usb::UsbConfig previous = d_.usb->config();
    if (d_.save_usb && !d_.save_usb(cfg, e))
        return ApiService::fail(500, "io_error", path, e);

    if (!d_.usb->apply(cfg, e).is_ok()) {
        // The config was already written. Leaving it there would mean a 500
        // that nevertheless changes what the port does at the next boot --
        // "nothing was applied" has to be true on disk as well, not just in
        // RAM. If putting it back also fails there is nothing further we can
        // do, and saying so beats a reassuring message.
        std::string e2;
        if (d_.save_usb && !d_.save_usb(previous, e2))
            return ApiService::fail(500, "io_error", path,
                                    e + "; the previous setting could also not be restored (" + e2 +
                                    ") -- the stored USB configuration is now the one that failed");
        return ApiService::fail(500, "io_error", path, e);
    }

    Json out = usb_status_json(d_.usb->status());
    out.set("config", usb_config_json(d_.usb->config()));
    return ok(out);
}

Response NetApiService::usb_devices() const
{
    if (!d_.usb) return not_wired("/api/v1/usb/devices", "the USB host");
    return ok(usb_devices_json(d_.usb->status().devices));
}

// ---------------------------------------------------------- connectivity

Json NetApiService::net_views_policy() const
{
    return policy_json(d_.conn ? d_.conn->policy() : net::UplinkPolicy());
}

Response NetApiService::network_get() const
{
    if (!d_.conn) return not_wired("/api/v1/network", "connectivity management");
    Json j = network_json(d_.conn->status(), d_.conn->policy(), d_.conn->active_id());
    j.set("change", change_get().body);
    // The uplink list already carries cellular as one entry among three; this
    // is the detail a modem has and a cable does not -- operator, band, SIM.
    // Absent rather than null when there is no modem support in this build, so
    // a page can tell "this camera has no cellular" from "the modem is quiet".
    if (d_.cellular)
        j.set("cellular", cellular_network_json(d_.cellular->modem_status(),
                                                d_.cellular->config(),
                                                d_.cellular->link_state(),
                                                d_.cellular->state(),
                                                d_.cellular->has_internet()));
    return ok(j);
}

Response NetApiService::policy_patch(const std::string& body)
{
    const std::string path = "/api/v1/network/policy";
    if (!d_.conn) return not_wired(path, "connectivity management");

    Json j; Response err;
    if (!parse_body(body, path, j, err)) return err;

    net::UplinkPolicy p = d_.conn->policy();
    std::string e;
    if (!policy_from_json(j, p, e)) return ApiService::fail(422, "invalid_value", path, e);

    if (d_.save_policy && !d_.save_policy(p, e))
        return ApiService::fail(500, "io_error", path, e);

    d_.conn->set_policy(p);
    // Act on it at once: a user who just moved Ethernet to the top expects the
    // status page to agree, not to wait for the next poll.
    d_.conn->evaluate();
    return ok(policy_json(d_.conn->policy()));
}

// ------------------------------------------------------------------ WiFi

Response NetApiService::wifi_get() const
{
    const std::string path = "/api/v1/network/wifi";
    if (!d_.wifi) return not_wired(path, "WiFi");

    const net::WifiCapabilities caps = d_.wifi->capabilities();
    net::WifiNetwork connected;
    const bool have = d_.wifi->station_status(connected);
    return ok(wifi_status_json(caps, d_.wifi->mode(), d_.wifi->state(), have ? &connected : nullptr));
}

Response NetApiService::wifi_scan()
{
    const std::string path = "/api/v1/network/wifi/scan";
    if (!d_.wifi) return not_wired(path, "WiFi");

    const net::WifiCapabilities caps = d_.wifi->capabilities();
    if (!caps.scan_usable())
        return ApiService::fail(409, "unsupported", path, "this radio cannot scan");

    std::vector<net::WifiNetwork> found;
    if (!d_.wifi->scan(found).is_ok())
        return ApiService::fail(503, "unavailable", path, "the scan did not complete");
    return ok(wifi_scan_json(found));
}

Response NetApiService::wifi_station(const std::string& body)
{
    const std::string path = "/api/v1/network/wifi/station";
    if (!d_.wifi) return not_wired(path, "WiFi");

    const net::WifiCapabilities caps = d_.wifi->capabilities();
    if (!caps.station_usable())
        return ApiService::fail(409, "unsupported", path,
                                caps.present ? "station mode is not usable on this build"
                                             : "no WiFi radio is present");

    Json j; Response err;
    if (!parse_body(body, path, j, err)) return err;

    net::WifiStationConfig cfg;
    std::string e;
    if (!wifi_station_from_json(j, cfg, e)) return ApiService::fail(422, "invalid_value", path, e);

    Json cand = Json::object();
    cand.set("kind", Json::string("wifi-station"));
    cand.set("config", j);
    return stage(path, cand.dump());
}

Response NetApiService::wifi_ap(const std::string& body)
{
    const std::string path = "/api/v1/network/wifi/ap";
    if (!d_.wifi) return not_wired(path, "WiFi");

    const net::WifiCapabilities caps = d_.wifi->capabilities();
    // ATTEMPTABLE, not verified. Refusing every board whose driver we have not
    // interrogated would make the feature unreachable on all of them,
    // including the one in front of us; claiming it works would be a promise
    // nothing here has earned. So the attempt is allowed and the adapter
    // VERIFIES the result -- a radio that cannot do AP mode fails there, with
    // hostapd's own reason, rather than silently serving nothing.
    if (!caps.ap_attemptable()) {
        // The adapter normally fills in which half is missing. When it has not
        // -- an adapter that does not implement describe() -- the reason is
        // derived here rather than falling back to "not available", which
        // tells the user nothing they can act on.
        std::string why = caps.ap_unavailable_reason;
        if (why.empty()) {
            if (!caps.present)                    why = "no WiFi radio is present";
            else if (!caps.hostapd_available)     why = "hostapd is not in this image";
            else if (!caps.dhcp_server_available) why = "no DHCP server in this image; "
                                                        "clients would associate and get no address";
            else if (caps.driver_ap_known && !caps.driver_ap)
                                                  why = "this radio's driver does not support access point mode";
            else                                  why = "access point mode is not available";
        }
        return ApiService::fail(409, "unsupported", path, why);
    }

    Json j; Response err;
    if (!parse_body(body, path, j, err)) return err;

    net::WifiApConfig cfg;
    std::string e;
    if (!wifi_ap_from_json(j, cfg, e)) return ApiService::fail(422, "invalid_value", path, e);

    if (cfg.dhcp_server && !caps.dhcp_server_available)
        return ApiService::fail(409, "unsupported", path,
                                "no DHCP server in this image; clients would get no address");

    Json cand = Json::object();
    cand.set("kind", Json::string("wifi-ap"));
    cand.set("config", j);
    return stage(path, cand.dump());
}

// ------------------------------------------------------------- Mobilfunk

Response NetApiService::cellular_get() const
{
    const std::string path = "/api/v1/network/cellular";
    if (!d_.cellular) return not_wired(path, "cellular");
    return ok(cellular_network_json(d_.cellular->modem_status(),
                                    d_.cellular->config(),
                                    d_.cellular->link_state(),
                                    d_.cellular->state(),
                                    d_.cellular->has_internet()));
}

Response NetApiService::cellular_presets() const
{
    Json j = Json::object();
    j.set("presets", cellular_presets_json());
    return ok(j);
}

Response NetApiService::cellular_patch(const std::string& body)
{
    const std::string path = "/api/v1/network/cellular";
    if (!d_.cellular) return not_wired(path, "cellular");

    Json j; Response err;
    if (!parse_body(body, path, j, err)) return err;

    // Validated against the CURRENT configuration, because a PATCH is partial:
    // a missing simPin means "leave it alone", not "clear it". Getting that
    // backwards would erase the PIN every time the user changed the APN.
    cellular::CellularConfig next = d_.cellular->config();
    std::string e;
    if (!cellular_config_from_json(j, next, e))
        return ApiService::fail(422, "invalid_value", path, e);

    // Staged like every other network change, and for a reason that only
    // arrived with AP-M5: cellular now takes part in uplink selection, so
    // switching it on can move the default route off Ethernet -- and whoever
    // is administering the camera through that route would be the one who can
    // no longer confirm anything.
    //
    // What the rollback does NOT do is re-drive the modem. It restores a
    // configuration; AT+CPIN is sent by SimManager, which refuses a second
    // attempt for a PIN value it has already had rejected, and that note is
    // keyed to the value and survives the round trip. So an undone change
    // cannot spend a SIM attempt, which is the one thing a rollback here must
    // never cost.
    Json cand = Json::object();
    cand.set("kind", Json::string("cellular"));
    cand.set("config", j);
    return stage(path, cand.dump());
}

// ------------------------------------------------------- staged changes

Response NetApiService::stage(const std::string& path, const std::string& candidate)
{
    if (!d_.txn)    return not_wired(path, "staged network changes");
    if (!d_.now_ms) return ApiService::fail(500, "internal", path, "no clock is wired");

    std::lock_guard<std::mutex> g(m_);

    uint64_t token = 0;
    std::string e;
    if (!d_.txn->begin(candidate, d_.now_ms(), d_.confirm_window_ms, token, e)) {
        staged_candidate_.clear();
        // 409, not 500: every reason begin() refuses is a state the caller can
        // see and act on -- something else is pending, or there is no
        // known-good configuration to fall back to yet.
        return ApiService::fail(409, "conflict", path, e);
    }
    staged_candidate_ = candidate;

    Json j = Json::object();
    j.set("pending", Json::boolean(true));
    j.set("token", Json::string(std::to_string(token)));
    j.set("confirm_within_ms", Json::integer((long long)d_.confirm_window_ms));
    j.set("confirm_url", Json::string("/api/v1/network/change/" + std::to_string(token) + "/confirm"));
    // Spelled out because it is the whole point: a client that loses the
    // connection here must NOT retry blindly, it must wait for the rollback.
    j.set("note", Json::string("this change is undone automatically unless it is confirmed"));
    return Response{202, j};
}

Response NetApiService::change_get() const
{
    Json j = Json::object();
    const bool pending = d_.txn && d_.txn->pending();
    j.set("pending", Json::boolean(pending));
    if (pending) {
        j.set("token", Json::string(std::to_string(d_.txn->token())));
        j.set("remaining_ms", Json::integer((long long)d_.txn->remaining_ms(d_.now_ms ? d_.now_ms() : 0)));
    }
    return ok(j);
}

Response NetApiService::change_confirm(const std::string& token_text)
{
    const std::string path = "/api/v1/network/change/" + token_text + "/confirm";
    if (!d_.txn) return not_wired(path, "staged network changes");

    std::lock_guard<std::mutex> g(m_);

    const uint64_t token = std::strtoull(token_text.c_str(), nullptr, 10);
    std::string e;
    if (!d_.txn->confirm(token, e))
        return ApiService::fail(409, "conflict", path, e);

    // Only NOW does the change become permanent. Persisting it when it was
    // applied would leave nothing for a rollback to go back to.
    const std::string confirmed = staged_candidate_;
    staged_candidate_.clear();
    if (d_.on_confirmed && !confirmed.empty()) d_.on_confirmed(confirmed);

    Json j = Json::object();
    j.set("pending", Json::boolean(false));
    j.set("confirmed", Json::boolean(true));
    return ok(j);
}

bool NetApiService::tick()
{
    if (!d_.txn || !d_.now_ms) return false;
    const bool rolled_back = d_.txn->tick(d_.now_ms());
    if (rolled_back) {
        std::lock_guard<std::mutex> g(m_);
        staged_candidate_.clear();   // it was undone, it must not be confirmable
    }
    return rolled_back;
}

}} // namespace machino::api
