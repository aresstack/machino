// AP3 (Feature 2): die IPsec-Routen. Duenn -- Validierung und Persistenz
// wohnen in core/net/ipsec_config, der Lifecycle in IpsecService; hier wird
// nur JSON <-> IpsecConfig uebersetzt. Der PSK laeuft write-only durch:
// PUT nimmt "psk" an und reicht ihn weiter, keine Antwort und keine
// Fehlermeldung traegt ihn je.
#include "app/api/api_service.hpp"

namespace machino { namespace api {

namespace {

Json algo_array(const std::vector<std::string>& v)
{
    Json a = Json::array();
    for (const auto& s : v) a.push(Json::string(s));
    return a;
}

Json ipsec_config_json(const ipsec::IpsecConfig& c, bool psk_set)
{
    Json j = Json::object();
    j.set("enabled", Json::boolean(c.enabled));
    j.set("gateway", Json::string(c.gateway));
    j.set("port", Json::integer(c.port));
    j.set("underlay", Json::string(ipsec::underlay_name(c.underlay)));
    j.set("localId", Json::string(c.local_id));
    j.set("remoteId", Json::string(c.remote_id));
    j.set("localSubnet", Json::string(c.local_subnet));
    j.set("remoteSubnet", Json::string(c.remote_subnet));
    j.set("natT", Json::boolean(c.nat_t));
    j.set("dpdIntervalS", Json::integer(c.dpd_interval_s));
    j.set("ikeLifetimeS", Json::integer((long long)c.ike_lifetime_s));
    j.set("childLifetimeS", Json::integer((long long)c.child_lifetime_s));
    j.set("ikeEnc", algo_array(c.ike_enc));
    j.set("ikeHash", algo_array(c.ike_hash));
    j.set("ikeDh", algo_array(c.ike_dh));
    j.set("espEnc", algo_array(c.esp_enc));
    j.set("espHash", algo_array(c.esp_hash));
    j.set("pskSet", Json::boolean(psk_set));
    return j;
}

bool take_string(const Json& o, const char* k, std::string& out, std::string& err)
{
    const Json* v = o.get(k);
    if (!v) return true;
    if (!v->is_string()) { err = std::string(k) + ": String erwartet"; return false; }
    out = v->as_string();
    return true;
}

bool take_bool(const Json& o, const char* k, bool& out, std::string& err)
{
    const Json* v = o.get(k);
    if (!v) return true;
    if (!v->is_bool()) { err = std::string(k) + ": Boolean erwartet"; return false; }
    out = v->as_bool();
    return true;
}

bool take_int(const Json& o, const char* k, long long lo, long long hi,
              long long& out, std::string& err)
{
    const Json* v = o.get(k);
    if (!v) return true;
    if (!v->is_integer()) { err = std::string(k) + ": Ganzzahl erwartet"; return false; }
    const long long n = v->as_int();
    if (n < lo || n > hi) { err = std::string(k) + ": ausserhalb " + std::to_string(lo)
                                  + ".." + std::to_string(hi); return false; }
    out = n;
    return true;
}

bool take_algos(const Json& o, const char* k, std::vector<std::string>& out, std::string& err)
{
    const Json* v = o.get(k);
    if (!v) return true;
    if (!v->is_array()) { err = std::string(k) + ": Array erwartet"; return false; }
    std::vector<std::string> got;
    for (size_t i = 0; i < v->size(); ++i) {
        if (!v->at(i).is_string()) { err = std::string(k) + ": Array aus Strings erwartet"; return false; }
        got.push_back(v->at(i).as_string());
    }
    out = got;
    return true;
}

} // namespace

Response ApiService::ipsec_get()
{
    if (!ipsec_) return fail(404, "not_found", "/api/v1/ipsec", "ipsec ist auf dieser Plattform nicht verdrahtet");
    return Response{200, ipsec_config_json(ipsec_->config(), ipsec_->psk_set())};
}

Response ApiService::ipsec_put_config(const std::string& body)
{
    const char* path = "/api/v1/ipsec/config";
    if (!ipsec_) return fail(404, "not_found", path, "ipsec ist auf dieser Plattform nicht verdrahtet");

    Json in;
    std::string err;
    if (!Json::parse(body, in, err)) return fail(400, "bad_json", path, err);
    if (!in.is_object()) return fail(400, "bad_json", path, "Objekt erwartet");

    // Unbekannte Keys MIT NAMEN ablehnen -- derselbe Vertrag wie patch_config:
    // ein Tippfehler darf nicht leise als "gespeichert" durchgehen.
    static const char* known[] = {"enabled","gateway","port","underlay","localId","remoteId",
                                  "localSubnet","remoteSubnet","natT","dpdIntervalS",
                                  "ikeLifetimeS","childLifetimeS",
                                  "ikeEnc","ikeHash","ikeDh","espEnc","espHash","psk"};
    for (const auto& m : in.members()) {
        bool ok = false;
        for (const char* k : known) if (m.first == k) { ok = true; break; }
        if (!ok) return fail(400, "unknown_field", path, "unbekanntes Feld: " + m.first);
    }

    // Overlay auf den persistierten Stand: fehlende Felder bleiben, ein
    // PUT ohne "psk" laesst den vorhandenen PSK unangetastet (write-only).
    ipsec::IpsecConfig c = ipsec_->config();
    long long port = c.port, dpd = c.dpd_interval_s;
    long long ikeLt = c.ike_lifetime_s, childLt = c.child_lifetime_s;
    std::string underlay = ipsec::underlay_name(c.underlay), psk;

    if (!take_bool(in, "enabled", c.enabled, err) ||
        !take_string(in, "gateway", c.gateway, err) ||
        !take_int(in, "port", 1, 65535, port, err) ||
        !take_string(in, "underlay", underlay, err) ||
        !take_string(in, "localId", c.local_id, err) ||
        !take_string(in, "remoteId", c.remote_id, err) ||
        !take_string(in, "localSubnet", c.local_subnet, err) ||
        !take_string(in, "remoteSubnet", c.remote_subnet, err) ||
        !take_bool(in, "natT", c.nat_t, err) ||
        !take_int(in, "dpdIntervalS", 0, 3600, dpd, err) ||
        !take_int(in, "ikeLifetimeS", 0, 604800, ikeLt, err) ||
        !take_int(in, "childLifetimeS", 0, 604800, childLt, err) ||
        !take_algos(in, "ikeEnc", c.ike_enc, err) ||
        !take_algos(in, "ikeHash", c.ike_hash, err) ||
        !take_algos(in, "ikeDh", c.ike_dh, err) ||
        !take_algos(in, "espEnc", c.esp_enc, err) ||
        !take_algos(in, "espHash", c.esp_hash, err) ||
        !take_string(in, "psk", psk, err))
        return fail(400, "invalid_value", path, err);

    c.port = (uint16_t)port;
    c.dpd_interval_s = (int)dpd;
    c.ike_lifetime_s = (uint32_t)ikeLt;
    c.child_lifetime_s = (uint32_t)childLt;
    if (!ipsec::underlay_from_name(underlay, c.underlay))
        return fail(400, "invalid_value", path, "underlay: '" + underlay + "'");

    const std::string e = ipsec_->set_config(c, psk);
    if (!e.empty()) return fail(400, "invalid_value", path, e);

    Json j = Json::object();
    j.set("ok", Json::boolean(true));
    j.set("pskSet", Json::boolean(ipsec_->psk_set()));
    return Response{200, j};
}

Response ApiService::ipsec_connect()
{
    const char* path = "/api/v1/ipsec/connect";
    if (!ipsec_) return fail(404, "not_found", path, "ipsec ist auf dieser Plattform nicht verdrahtet");
    const std::string e = ipsec_->connect();
    if (!e.empty()) return fail(409, "conflict", path, e);
    Json j = Json::object(); j.set("ok", Json::boolean(true));
    return Response{200, j};
}

Response ApiService::ipsec_disconnect()
{
    const char* path = "/api/v1/ipsec/disconnect";
    if (!ipsec_) return fail(404, "not_found", path, "ipsec ist auf dieser Plattform nicht verdrahtet");
    const std::string e = ipsec_->disconnect();
    if (!e.empty()) return fail(409, "conflict", path, e);
    Json j = Json::object(); j.set("ok", Json::boolean(true));
    return Response{200, j};
}

Response ApiService::ipsec_status()
{
    if (!ipsec_) return fail(404, "not_found", "/api/v1/ipsec/status", "ipsec ist auf dieser Plattform nicht verdrahtet");
    const ipsec::VpnStatus st = ipsec_->status();
    Json j = Json::object();
    j.set("daemonRunning", Json::boolean(st.daemon_running));
    j.set("state", Json::string(ipsec::vpn_state_name(st.state)));
    // AP7: der explizite Runtime-Zustand + Reconnect-Sicht.
    j.set("runtimeState", Json::string(ipsec::vpn_runtime_state_name(st.runtime)));
    j.set("manualStop", Json::boolean(st.manual_stop));
    if (st.reconnect_attempt > 0) j.set("reconnectAttempt", Json::integer(st.reconnect_attempt));
    if (st.state == ipsec::VpnState::Failed) {
        Json f = Json::object();
        f.set("code", Json::string(ipsec::vpn_failure_name(st.failure)));
        f.set("lastNotify", Json::integer((long long)st.last_notify));
        j.set("failure", f);
    }
    if (!st.raw_state.empty()) j.set("rawState", Json::string(st.raw_state));
    if (!st.gateway.empty()) j.set("gateway", Json::string(st.gateway));
    if (!st.interface_name.empty()) j.set("interface", Json::string(st.interface_name));
    if (!st.local_ts.empty()) j.set("localTs", Json::string(st.local_ts));
    if (!st.remote_ts.empty()) j.set("remoteTs", Json::string(st.remote_ts));
    j.set("natT", Json::boolean(st.nat_t));
    j.set("natDetected", Json::boolean(st.nat_detected));
    // AP5: Underlay-Fakten. requested aus der Config, actual/interface/ipv4/
    // peer aus der Session, ike/espTransport GEMESSEN vom Daemon. Keine
    // Cellular-Secrets (APN, PIN) -- die gehoeren dieser Route nicht.
    j.set("requestedUnderlay", Json::string(st.requested_underlay));
    if (!st.actual_underlay.empty())    j.set("actualUnderlay", Json::string(st.actual_underlay));
    if (!st.underlay_interface.empty()) j.set("underlayInterface", Json::string(st.underlay_interface));
    if (!st.underlay_ipv4.empty())      j.set("underlayIpv4", Json::string(st.underlay_ipv4));
    if (!st.peer_ipv4.empty())          j.set("peerIpv4", Json::string(st.peer_ipv4));
    if (!st.ike_transport.empty())      j.set("ikeTransport", Json::string(st.ike_transport));
    if (!st.esp_transport.empty())      j.set("espTransport", Json::string(st.esp_transport));
    // AP6: die INSTALLIERTEN Routen (Daemon-Wahrheit), source tsr|cp getrennt.
    Json routes = Json::array();
    for (const auto& r : st.routes) {
        Json ro = Json::object();
        ro.set("prefix", Json::string(r.prefix));
        ro.set("source", Json::string(r.source));
        ro.set("device", Json::string(r.device));
        routes.push(ro);
    }
    j.set("routes", routes);
    if (st.full_tunnel_refused) j.set("fullTunnelRefused", Json::boolean(true));
    if (!st.child.empty()) j.set("child", Json::string(st.child));
    j.set("childGeneration", Json::integer((long long)st.child_generation));
    j.set("ikeGeneration", Json::integer((long long)st.ike_generation));
    j.set("uptimeS", Json::integer((long long)st.uptime_s));
    j.set("txPackets", Json::integer((long long)st.tx_packets));
    j.set("txBytes", Json::integer((long long)st.tx_bytes));
    j.set("rxPackets", Json::integer((long long)st.rx_packets));
    j.set("rxBytes", Json::integer((long long)st.rx_bytes));
    return Response{200, j};
}

}} // namespace machino::api
