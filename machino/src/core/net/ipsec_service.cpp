#include "core/net/ipsec_service.hpp"

#include <cstdio>
#include <cstdlib>

namespace machino { namespace ipsec {

namespace {

std::string read_file(const std::string& path, bool& existed)
{
    existed = false;
    FILE* f = ::fopen(path.c_str(), "rb");
    if (!f) return {};
    existed = true;
    std::string out;
    char buf[4096];
    size_t n;
    while ((n = ::fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
    ::fclose(f);
    return out;
}

std::string trim(const std::string& s)
{
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

// weirdike_state_str-Namen -> VpnState. Ein UNBEKANNTER Name wird Failed,
// nicht "irgendwas Optimistisches": lieber sichtbar falsch als leise gruen.
VpnState state_from_raw(const std::string& raw)
{
    if (raw == "IDLE")                 return VpnState::Disconnected;
    if (raw == "SA_INIT_SENT")         return VpnState::Connecting;
    if (raw == "SA_INIT_DONE")         return VpnState::Connecting;
    if (raw == "AUTH_SENT")            return VpnState::Connecting;
    if (raw == "IKE_SA_ESTABLISHED")   return VpnState::IkeEstablished;
    if (raw == "CHILD_SA_ESTABLISHED") return VpnState::ChildEstablished;
    if (raw == "CLOSED")               return VpnState::Disconnected;
    return VpnState::Failed;
}

} // namespace

const char* vpn_state_name(VpnState s)
{
    switch (s) {
        case VpnState::Disabled:         return "disabled";
        case VpnState::Disconnected:     return "disconnected";
        case VpnState::Connecting:       return "connecting";
        case VpnState::IkeEstablished:   return "ikeEstablished";
        case VpnState::ChildEstablished: return "childEstablished";
        case VpnState::Failed:           return "failed";
    }
    return "failed";
}

const char* vpn_failure_name(VpnFailure f)
{
    switch (f) {
        case VpnFailure::None:                 return "none";
        case VpnFailure::AuthenticationFailed: return "authenticationFailed";
        case VpnFailure::NoProposalChosen:     return "noProposalChosen";
        case VpnFailure::TsUnacceptable:       return "tsUnacceptable";
        case VpnFailure::TransportTimeout:     return "transportTimeout";
        case VpnFailure::UnderlayLost:         return "underlayLost";
        case VpnFailure::Other:                return "other";
    }
    return "other";
}

VpnStatus parse_status(const std::string& text, bool daemon_running, bool enabled)
{
    VpnStatus st;
    st.daemon_running = daemon_running;
    if (!daemon_running) {
        st.state = enabled ? VpnState::Disconnected : VpnState::Disabled;
        return st;
    }

    size_t pos = 0;
    while (pos < text.size()) {
        size_t eol = text.find('\n', pos);
        std::string line = trim(text.substr(
            pos, eol == std::string::npos ? std::string::npos : eol - pos));
        pos = eol == std::string::npos ? text.size() : eol + 1;
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = line.substr(0, eq), v = line.substr(eq + 1);

        if      (k == "state")            st.raw_state = v;
        else if (k == "gateway")          st.gateway = v;
        else if (k == "interface")        st.interface_name = v;
        else if (k == "local_ts")         st.local_ts = v;
        else if (k == "remote_ts")        st.remote_ts = v;
        else if (k == "natt")             st.nat_t = (v == "yes" || v == "1" || v == "true");
        else if (k == "nat_detected")     st.nat_detected = (v == "yes" || v == "1" || v == "true");
        else if (k == "child")            st.child = v;
        else if (k == "child_generation") st.child_generation = (uint32_t)strtoul(v.c_str(), nullptr, 10);
        else if (k == "ike_generation")   st.ike_generation = (uint32_t)strtoul(v.c_str(), nullptr, 10);
        else if (k == "last_notify")      st.last_notify = (uint32_t)strtoul(v.c_str(), nullptr, 10);
        else if (k == "uptime_s")         st.uptime_s = (uint32_t)strtoul(v.c_str(), nullptr, 10);
        else if (k == "tx_packets")       st.tx_packets = strtoull(v.c_str(), nullptr, 10);
        else if (k == "tx_bytes")         st.tx_bytes = strtoull(v.c_str(), nullptr, 10);
        else if (k == "rx_packets")       st.rx_packets = strtoull(v.c_str(), nullptr, 10);
        else if (k == "rx_bytes")         st.rx_bytes = strtoull(v.c_str(), nullptr, 10);
        else if (k == "ike_transport")    st.ike_transport = v;
        else if (k == "esp_transport")    st.esp_transport = v;
        // unbekannte Keys: ignorieren -- der Daemon darf wachsen
    }

    st.state = state_from_raw(st.raw_state);

    // Fehlerklassifikation NUR im Fehlerfall, aus WeirdIKEs eigener Diag
    // (last_notify), nicht aus Vermutungen. 0 = kein Notify = Transport.
    if (st.state == VpnState::Failed) {
        switch (st.last_notify) {
            case 24: st.failure = VpnFailure::AuthenticationFailed; break;
            case 14: st.failure = VpnFailure::NoProposalChosen; break;
            case 38: st.failure = VpnFailure::TsUnacceptable; break;
            case 0:  st.failure = VpnFailure::TransportTimeout; break;
            default: st.failure = VpnFailure::Other; break;
        }
    }
    return st;
}

IpsecService::IpsecService(IIpsecBackend& backend,
                           std::string machino_conf_path,
                           std::string daemon_conf_path,
                           IIpsecUplinks* uplinks)
    : backend_(backend),
      machino_path_(std::move(machino_conf_path)),
      daemon_path_(std::move(daemon_conf_path)),
      uplinks_(uplinks)
{
}

IpsecConfig IpsecService::config() const
{
    bool existed = false;
    const std::string text = read_file(machino_path_, existed);
    IpsecConfig c;
    std::string err;
    if (existed && !from_machino_conf(text, c, err)) {
        // Kaputte Datei: Defaults melden, aber NICHT stillschweigend
        // ueberschreiben -- das passiert erst beim naechsten set_config.
        return IpsecConfig{};
    }
    return c;
}

bool IpsecService::psk_set() const
{
    bool existed = false;
    const std::string text = read_file(daemon_path_, existed);
    if (!existed) return false;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t eol = text.find('\n', pos);
        std::string line = trim(text.substr(
            pos, eol == std::string::npos ? std::string::npos : eol - pos));
        pos = eol == std::string::npos ? text.size() : eol + 1;
        if (line.rfind("psk", 0) == 0) {
            size_t eq = line.find('=');
            if (eq != std::string::npos && !trim(line.substr(eq + 1)).empty()) return true;
        }
    }
    return false;
}

std::string IpsecService::set_config(const IpsecConfig& c, const std::string& psk_or_empty)
{
    const std::string ve = validate(c);
    if (!ve.empty()) return ve;
    const std::string pe = psk_check(psk_or_empty);
    if (!pe.empty()) return pe;

    bool existed = false;
    const std::string old_daemon = read_file(daemon_path_, existed);
    bool psk_present = false;
    const std::string daemon_conf = to_weirdike_conf(c, psk_or_empty, old_daemon, &psk_present);

    if (c.enabled && !psk_present)
        return "psk: erforderlich, wenn enabled=true (write-only; einmal setzen genuegt)";

    // Reihenfolge: erst die Daemon-Datei (traegt das Secret), dann die
    // machino-Wahrheit. Schlaegt Schritt 2 fehl, ist der alte machino-Stand
    // noch da und der Daemon-Conf lediglich voraus -- kein halber Zustand,
    // der enabled=true ohne PSK behauptet.
    std::string err;
    if (!write_atomic_0600(daemon_path_, daemon_conf, err))
        return "weirdike.conf: " + err;
    if (!write_atomic_0600(machino_path_, to_machino_conf(c), err))
        return "ipsec.conf: " + err;
    return {};
}

std::string IpsecService::connect()
{
    const IpsecConfig c = config();
    if (!c.enabled) return "ipsec ist deaktiviert (enabled=false)";
    if (c.gateway.empty()) return "kein Gateway konfiguriert";
    if (!psk_set()) return "kein PSK gesetzt";
    if (backend_.daemon_running()) return {};   // idempotent

    session_ = Session{};

    // AP5 §3: Underlay waehlen. Fuer cellular/ethernet/wifi genau den einen
    // (unbrauchbar -> VERWEIGERN, kein stiller Wechsel); fuer auto das, was
    // die bestehende Machino-Uplink-Policy jetzt faehrt — hier wird KEINE
    // zweite Failover-Policy gebaut.
    UnderlayView uv;
    if (uplinks_) {
        std::string uerr;
        if (!uplinks_->select(c.underlay, uv, uerr))
            return uerr.empty() ? "kein nutzbares Underlay" : uerr;
        if (!uv.usable || uv.ipv4.empty() || uv.ifname.empty())
            return std::string("underlay '") + underlay_name(c.underlay)
                   + "': nicht nutzbar (keine Adresse)";
    } else if (c.underlay != Underlay::Auto) {
        return std::string("underlay '") + underlay_name(c.underlay)
               + "': kein Uplink-Provider verdrahtet";
    }

    // AP5 §6: DNS EINMAL, vor dem Tunnel. Die Session merkt sich die Adresse;
    // Rekey laeuft im Daemon gegen dieselbe (er bekommt das Literal).
    std::string peer_ip;
    if (!backend_.resolve4(c.gateway, peer_ip) || peer_ip.empty())
        return "gateway '" + c.gateway + "': DNS-Aufloesung fehlgeschlagen";

    // AP5 §5/§8: Peer-Hostroute ZUERST, auf dem gewaehlten Underlay. Sie
    // gehoert dem Service und ueberlebt jede spaeter verhandelte Tunnelroute.
    std::string err;
    if (uplinks_) {
        if (!backend_.add_peer_route(peer_ip, uv.ifname, uv.gateway, err))
            return "Peer-Route: " + (err.empty() ? std::string("fehlgeschlagen") : err);
        session_.peer_route = true;
    }

    // Daemon-Datei mit den SESSION-Werten neu erzeugen (PSK wird write-only
    // weitergetragen): aufgeloestes Gateway-Literal + konkrete Bindung.
    bool existed = false;
    const std::string old_daemon = read_file(daemon_path_, existed);
    SessionNet net;
    net.gateway_ip = peer_ip;
    net.bind_ip = uv.ipv4;
    net.bind_dev = uv.ifname;
    bool psk_present = false;
    const std::string dconf = to_weirdike_conf(c, "", old_daemon, &psk_present, &net);
    if (!psk_present) { teardown_session_(false); return "kein PSK gesetzt"; }
    if (!write_atomic_0600(daemon_path_, dconf, err)) {
        teardown_session_(false);
        return "weirdike.conf: " + err;
    }

    if (!backend_.start_daemon(err)) {
        teardown_session_(false);
        return err.empty() ? "Daemon-Start fehlgeschlagen" : err;
    }

    session_.active = true;
    session_.underlay_kind = uplinks_ ? uv.kind : "";
    session_.ifname = uv.ifname;
    session_.ipv4 = uv.ipv4;
    session_.gateway_ip = uv.gateway;
    session_.peer_ip = peer_ip;
    return {};
}

void IpsecService::teardown_session_(bool lost)
{
    if (session_.peer_route) {
        std::string err;
        backend_.del_peer_route(session_.peer_ip, session_.ifname, session_.gateway_ip, err);
    }
    const std::string kind = session_.underlay_kind;
    session_ = Session{};
    session_.lost = lost;
    if (lost) session_.underlay_kind = kind;   // fuer die Fehlermeldung im Status
}

std::string IpsecService::disconnect()
{
    std::string err, out;
    if (backend_.daemon_running()) {
        if (!backend_.stop_daemon(err))
            out = err.empty() ? "Daemon-Stopp fehlgeschlagen" : err;
    }
    // Die Peer-Route verschwindet auch dann, wenn der Stopp scheiterte —
    // eine Route zu einem toten Tunnel ist nur ein Blackhole mit Namen.
    teardown_session_(false);
    return out;
}

void IpsecService::tick()
{
    if (!session_.active || !uplinks_) return;

    UnderlayView uv;
    std::string uerr;
    IpsecConfig c = config();
    const bool ok = uplinks_->select(c.underlay, uv, uerr) && uv.usable;

    // Session ist an das KONKRETE Interface + Adresse gebunden. Anderes
    // Interface oder andere Adresse = das alte Underlay ist weg; die SA
    // einfach umzuziehen waere vorgetaeuschtes MOBIKE.
    if (ok && uv.ifname == session_.ifname && uv.ipv4 == session_.ipv4) return;

    std::string err;
    backend_.stop_daemon(err);
    teardown_session_(true);
}

VpnStatus IpsecService::status()
{
    // EINE ctl-Verbindung ist die Wahrheit fuer beides: "laeuft" UND der
    // Text. Zwei getrennte Roundtrips (daemon_running + ctl_status) hatten
    // ein Race -- Daemon stirbt dazwischen, und ein leerer Text mit
    // running=true wuerde zu failed/transportTimeout fantasiert.
    std::string text;
    const bool running = backend_.ctl_status(text) && !text.empty();
    const IpsecConfig c = config();
    VpnStatus st = parse_status(text, running, c.enabled);

    // AP5: Underlay-Fakten der Session dazu. requested aus der Config,
    // actual nur solange die Session lebt — keine Fantasie nach dem Ende.
    st.requested_underlay = underlay_name(c.underlay);
    if (session_.active) {
        st.actual_underlay = session_.underlay_kind;
        st.underlay_interface = session_.ifname;
        st.underlay_ipv4 = session_.ipv4;
        st.peer_ipv4 = session_.peer_ip;
    }
    if (session_.lost && !running) {
        st.state = VpnState::Failed;
        st.failure = VpnFailure::UnderlayLost;
    }
    return st;
}

}} // namespace machino::ipsec
