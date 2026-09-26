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

const char* vpn_runtime_state_name(VpnRuntimeState s)
{
    switch (s) {
        case VpnRuntimeState::Disabled:         return "disabled";
        case VpnRuntimeState::Idle:             return "idle";
        case VpnRuntimeState::Resolving:        return "resolving";
        case VpnRuntimeState::Binding:          return "binding";
        case VpnRuntimeState::IkeConnecting:    return "ikeConnecting";
        case VpnRuntimeState::IkeEstablished:   return "ikeEstablished";
        case VpnRuntimeState::ChildEstablished: return "childEstablished";
        case VpnRuntimeState::DataPlaneUp:      return "dataPlaneUp";
        case VpnRuntimeState::Rekeying:         return "rekeying";
        case VpnRuntimeState::Disconnecting:    return "disconnecting";
        case VpnRuntimeState::Failed:           return "failed";
    }
    return "failed";
}

uint32_t reconnect_delay_ms(int attempt)
{
    switch (attempt) {
        case 0:
        case 1:  return 0;
        case 2:  return 2000;
        case 3:  return 5000;
        case 4:  return 10000;
        default: return 30000;
    }
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
        else if (k == "auth")             st.auth = v;
        else if (k == "full_tunnel_refused") st.full_tunnel_refused = (v == "yes" || v == "1" || v == "true");
        else if (k == "route") {
            // "prefix source device" (space-separated), vom Daemon.
            VpnStatus::Route r;
            size_t s1 = v.find(' ');
            r.prefix = v.substr(0, s1);
            if (s1 != std::string::npos) {
                size_t s2 = v.find(' ', s1 + 1);
                r.source = v.substr(s1 + 1, s2 == std::string::npos ? std::string::npos : s2 - s1 - 1);
                if (s2 != std::string::npos) r.device = v.substr(s2 + 1);
            }
            if (!r.prefix.empty()) st.routes.push_back(r);
        }
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

// AP7: der explizite Runtime-Zustand aus (Daemon-Status + Session). Ein
// Child mit mindestens einer Route ist DataPlaneUp; ohne Route bleibt es
// ChildEstablished (AP6 kann eine Route ablehnen, ohne dass die SA faellt).
VpnRuntimeState IpsecService::derive_runtime_(const VpnStatus& s) const
{
    if (s.state == VpnState::Disabled) return VpnRuntimeState::Disabled;
    if (s.state == VpnState::Failed)   return VpnRuntimeState::Failed;
    if (!s.daemon_running) return session_.active ? VpnRuntimeState::Binding
                                                  : VpnRuntimeState::Idle;
    switch (s.state) {
        case VpnState::Connecting:     return VpnRuntimeState::IkeConnecting;
        case VpnState::IkeEstablished: return VpnRuntimeState::IkeEstablished;
        case VpnState::ChildEstablished:
            if (rekey_until_ms_ != 0) return VpnRuntimeState::Rekeying;
            return s.routes.empty() ? VpnRuntimeState::ChildEstablished
                                    : VpnRuntimeState::DataPlaneUp;
        case VpnState::Disconnected:   return VpnRuntimeState::Idle;
        default:                       return VpnRuntimeState::Idle;
    }
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

// "key = <nonempty>" in der Daemon-Datei? (Presence, nie der Wert.)
bool IpsecService::has_daemon_key_(const char* key) const
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
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        if (trim(line.substr(0, eq)) == key && !trim(line.substr(eq + 1)).empty()) return true;
    }
    return false;
}

bool IpsecService::psk_set() const          { return has_daemon_key_("psk"); }
bool IpsecService::eap_password_set() const  { return has_daemon_key_("eap_password"); }

std::string IpsecService::ca_pem_path_() const
{
    const size_t slash = daemon_path_.find_last_of("/\\");
    const std::string dir = slash == std::string::npos ? std::string() : daemon_path_.substr(0, slash + 1);
    return dir + "ca.pem";
}
std::string IpsecService::extra_pem_path_() const
{
    const size_t slash = daemon_path_.find_last_of("/\\");
    const std::string dir = slash == std::string::npos ? std::string() : daemon_path_.substr(0, slash + 1);
    return dir + "extra.pem";
}
bool IpsecService::file_exists_(const std::string& path)
{
    FILE* f = ::fopen(path.c_str(), "rb");
    if (!f) return false;
    const int ch = ::fgetc(f);          // leere Datei zaehlt nicht als "gesetzt"
    ::fclose(f);
    return ch != EOF;
}
bool IpsecService::ca_pem_set() const    { return file_exists_(ca_pem_path_()); }
bool IpsecService::extra_pem_set() const { return file_exists_(extra_pem_path_()); }
bool IpsecService::host_store_available() const { return backend_.host_store_available(); }

std::string IpsecService::set_config(const IpsecConfig& c, const IpsecSecrets& secrets)
{
    const std::string ve = validate(c);
    if (!ve.empty()) return ve;
    // Beide Secrets folgen denselben Regeln (kein \n, kein Randraum, <=64).
    std::string pe = psk_check(secrets.psk);
    if (!pe.empty()) return pe;
    pe = psk_check(secrets.eap_password);
    if (!pe.empty()) return "eapPassword" + pe.substr(pe.find(':'));

    // AP9: neue PEM-Inhalte VOR der Daemon-Datei schreiben (0600), damit die
    // ca_pem_file/extra_pem_file-Zeilen auf existierende Dateien zeigen.
    std::string err;
    if (!secrets.ca_pem.empty() && !write_atomic_0600(ca_pem_path_(), secrets.ca_pem, err))
        return "ca.pem: " + err;
    if (!secrets.extra_pem.empty() && !write_atomic_0600(extra_pem_path_(), secrets.extra_pem, err))
        return "extra.pem: " + err;

    bool existed = false;
    const std::string old_daemon = read_file(daemon_path_, existed);
    bool cred_present = false;
    const std::string ca_file    = file_exists_(ca_pem_path_())    ? ca_pem_path_()    : std::string();
    const std::string extra_file = file_exists_(extra_pem_path_()) ? extra_pem_path_() : std::string();
    const std::string daemon_conf = to_weirdike_conf(c, secrets, old_daemon, &cred_present,
                                                     nullptr, ca_file, extra_file);

    if (c.enabled && !cred_present)
        return c.auth == Auth::EapMschapv2
                 ? "eapPassword: erforderlich, wenn enabled=true (write-only; einmal setzen genuegt)"
                 : "psk: erforderlich, wenn enabled=true (write-only; einmal setzen genuegt)";

    // Reihenfolge: erst die Daemon-Datei (traegt das Secret), dann die
    // machino-Wahrheit. Schlaegt Schritt 2 fehl, ist der alte machino-Stand
    // noch da und der Daemon-Conf lediglich voraus -- kein halber Zustand.
    if (!write_atomic_0600(daemon_path_, daemon_conf, err))
        return "weirdike.conf: " + err;
    if (!write_atomic_0600(machino_path_, to_machino_conf(c), err))
        return "ipsec.conf: " + err;
    return {};
}

std::string IpsecService::connect()
{
    return connect_(false);
}

// prefer_cached: der Auto-Reconnect (tick) laeuft im Hauptthread des
// MEDIENdaemons. Ein blockierendes getaddrinfo() dort friert das Video ein,
// solange der Resolver braucht (bei gerade erst zurueckgekehrtem Underlay
// zweistellige Sekunden) -- das verletzt "kein VPN-Fehler darf Video
// herunterfahren". Deshalb nutzt der Reconnect die bereits aufgeloeste
// Peer-IP der letzten Sitzung wieder (AP5 §6: Rekey/dieselbe Sitzung nutzt
// dieselbe Adresse; nur ein bewusster connect() loest frisch auf).
std::string IpsecService::connect_(bool prefer_cached)
{
    const IpsecConfig c = config();
    if (!c.enabled) return "ipsec ist deaktiviert (enabled=false)";
    if (c.gateway.empty()) return "kein Gateway konfiguriert";
    // AP9: das passende Credential fuer den Auth-Modus.
    if (c.auth == Auth::EapMschapv2) {
        if (c.eap_user.empty())    return "kein EAP-Benutzer gesetzt";
        if (!eap_password_set())   return "kein EAP-Passwort gesetzt";
    } else {
        if (!psk_set())            return "kein PSK gesetzt";
    }
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
    // Rekey laeuft im Daemon gegen dieselbe (er bekommt das Literal). Beim
    // Auto-Reconnect die gecachte Adresse wiederverwenden (kein blockierendes
    // getaddrinfo im Medien-Hauptthread), sofern das Gateway unveraendert ist.
    std::string peer_ip;
    if (prefer_cached && cached_gateway_ == c.gateway && !cached_peer_ip_.empty()) {
        peer_ip = cached_peer_ip_;
    } else if (!backend_.resolve4(c.gateway, peer_ip) || peer_ip.empty()) {
        return "gateway '" + c.gateway + "': DNS-Aufloesung fehlgeschlagen";
    }

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
    // Secrets bleiben leer -> to_weirdike_conf traegt psk/eap_password aus der
    // alten Datei write-only weiter; die PEM-Pfade zeigen auf die Session-CA.
    IpsecSecrets carry;
    const std::string ca_file    = ca_pem_set()    ? ca_pem_path_()    : std::string();
    const std::string extra_file = extra_pem_set() ? extra_pem_path_() : std::string();
    bool cred_present = false;
    const std::string dconf = to_weirdike_conf(c, carry, old_daemon, &cred_present,
                                               &net, ca_file, extra_file);
    if (!cred_present) { teardown_session_(false); return "kein Credential gesetzt"; }
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

    // AP7: ein bewusster Connect hebt den manualStop auf und ist der Anker
    // fuer den Backoff-Reset (der endgueltige Reset kommt in tick(), sobald
    // der Tunnel STABIL steht -- ChildEstablished, nicht schon beim Start).
    manual_stop_ = false;
    reconnect_scheduled_ = false;
    cached_gateway_ = c.gateway;      // fuer den naechsten Auto-Reconnect
    cached_peer_ip_ = peer_ip;
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
    // AP7 §10/§11: ein Betreiber-Stopp verbietet Auto-Reconnect (bis zum
    // naechsten bewussten connect()). Der Daemon macht den geordneten Abbau
    // selbst (ctl "down" -> RFC-7296 DELETE, bounded, dann S99 stop).
    manual_stop_ = true;
    reconnect_scheduled_ = false;
    reconnect_attempt_ = 0;

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

// AP7 §9,§10: die Reconnect-Schleife. Aufgerufen aus dem Hauptthread mit der
// Uhr des Aufrufers. Baut bei Underlay-Verlust ab und plant nach einem
// WIEDERHERSTELLBAREN Fehlschlag einen Reconnect mit Backoff+Jitter.
void IpsecService::tick(uint32_t now_ms)
{
    const IpsecConfig c = config();

    // 1) Rekey-Fenster (nur fuer den Runtime-Zustand) auslaufen lassen.
    if (rekey_until_ms_ && (int32_t)(now_ms - rekey_until_ms_) >= 0) rekey_until_ms_ = 0;

    // 2) Laufende Session: Underlay-Verlust und DPD-Verlust erkennen.
    if (session_.active) {
        bool lost = false;
        if (uplinks_) {
            UnderlayView uv; std::string uerr;
            const bool ok = uplinks_->select(c.underlay, uv, uerr) && uv.usable;
            // an KONKRETES Interface + Adresse gebunden; Wechsel = altes weg.
            if (!ok || uv.ifname != session_.ifname || uv.ipv4 != session_.ipv4) lost = true;
        }
        // DPD-Verlust: der Daemon meldet FAILED, nachdem er lief.
        std::string text;
        const bool running = backend_.ctl_status(text) && !text.empty();
        if (!lost && running) {
            const VpnStatus s = parse_status(text, running, c.enabled);
            if (s.state == VpnState::Failed) lost = true;
            // Rekey sichtbar machen: Child-Generation ist gestiegen.
            if (s.child_generation > last_child_gen_ && last_child_gen_ != 0)
                rekey_until_ms_ = now_ms + 3000;
            if (s.child_generation) last_child_gen_ = s.child_generation;
            // stabil (Child steht) -> Backoff-Reset.
            if (s.state == VpnState::ChildEstablished) reconnect_attempt_ = 0;
        }
        if (!lost && !running) lost = true;   // Daemon unerwartet weg

        if (lost) {
            backend_.stop_daemon(text);
            teardown_session_(true);
            // Reconnect nur, wenn wiederherstellbar und nicht manuell gestoppt.
            schedule_reconnect_(now_ms, c);
        }
        return;
    }

    // 3) Kein Session, aber ein geplanter Reconnect ist faellig.
    if (reconnect_scheduled_ && !manual_stop_ &&
        (int32_t)(now_ms - next_reconnect_ms_) >= 0) {
        reconnect_scheduled_ = false;
        const std::string e = connect_(true); // gecachte Peer-IP, kein DNS-Stall
        if (!e.empty()) {
            // Fehlgeschlagener Versuch: naechsten planen (weiter hochzaehlen).
            schedule_reconnect_(now_ms, c);
        }
    }
}

// Plant den naechsten Reconnect, sofern sinnvoll. Terminale Ursachen
// (manualStop, ungueltige Config, fehlendes Credential) planen NICHTS.
void IpsecService::schedule_reconnect_(uint32_t now_ms, const IpsecConfig& c)
{
    if (manual_stop_ || !c.enabled) { reconnect_scheduled_ = false; return; }
    // AP9: das noetige Credential haengt am Auth-Modus -- ein EAP-Tunnel darf
    // NICHT an einem fehlenden PSK scheitern (und umgekehrt).
    const bool cred_ok = c.auth == Auth::EapMschapv2
                           ? (!c.eap_user.empty() && eap_password_set())
                           : psk_set();
    if (!validate(c).empty() || c.gateway.empty() || !cred_ok) {
        reconnect_scheduled_ = false; return;
    }
    reconnect_attempt_++;
    // xorshift-Jitter 0..25% des Delays (kein Secret; nur Herdenschutz).
    rng_ ^= rng_ << 13; rng_ ^= rng_ >> 17; rng_ ^= rng_ << 5;
    uint32_t base = reconnect_delay_ms(reconnect_attempt_);
    uint32_t jit = base ? (rng_ % (base / 4 + 1)) : 0;
    next_reconnect_ms_ = now_ms + base + jit;
    reconnect_scheduled_ = true;
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

    // AP7: Runtime-Zustand + Reconnect-Sicht.
    st.manual_stop = manual_stop_;
    st.reconnect_attempt = reconnect_scheduled_ ? reconnect_attempt_ : 0;
    st.runtime = derive_runtime_(st);
    return st;
}

}} // namespace machino::ipsec
