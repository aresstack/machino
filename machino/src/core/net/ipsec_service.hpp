// IpsecService (AP3): die machinod-Seite des VPN — Konfiguration, Persistenz,
// Lifecycle, Status. Er spricht mit weirdiked ueber die PROZESSGRENZE
// (PLATFORM.md): Konfig-Dateien, Init-Skript, Control-Socket. Kein
// WeirdIKE-Symbol in machinod; die IKEv2-Wahrheit kommt ausschliesslich aus
// weirdikeds Status (der sie 1:1 aus weirdike_state/get_diag zieht) — nie
// aus "Socket offen" oder eigener Fantasie.
#pragma once
#include "core/net/ipsec_config.hpp"

#include <string>
#include <vector>

namespace machino { namespace ipsec {

// Die Zustaende aus dem AP3-Vertrag. Connected bleibt AP4 vorbehalten
// (Datenpfad-Beweis); bis dahin ist ChildEstablished die Spitze.
enum class VpnState {
    Disabled,
    Disconnected,
    Connecting,       // SA_INIT_SENT / SA_INIT_DONE / AUTH_SENT
    IkeEstablished,
    ChildEstablished,
    Failed,
};
const char* vpn_state_name(VpnState s);

// AP7: der explizite Runtime-Lebenszyklus. Kombiniert die machinod-Phase
// (connect() ist mehrstufig) mit der Protokollwahrheit des Daemons. Der
// API/UI-Status wird HIERAUS abgeleitet, nicht aus verstreuten if-Flags.
enum class VpnRuntimeState {
    Disabled,        // enabled=false
    Idle,            // enabled, aber keine Session
    Resolving,       // connect(): DNS
    Binding,         // connect(): Underlay + Peer-Route + Daemonstart
    IkeConnecting,   // Daemon: SA_INIT/AUTH unterwegs
    IkeEstablished,  // Daemon: IKE steht, noch kein Child
    ChildEstablished,// Daemon: Child ausgehandelt
    DataPlaneUp,     // Child + mindestens eine Route installiert
    Rekeying,        // Child-Generation hat gerade gewechselt
    Disconnecting,   // geordneter Abbau laeuft
    Failed,          // Fehlgeschlagen (siehe VpnFailure)
};
const char* vpn_runtime_state_name(VpnRuntimeState s);

// AP7 §10: Reconnect-Backoff (rein, testbar). attempt ist 1-basiert.
//   1 -> 0ms (sofort), 2 -> 2s, 3 -> 5s, 4 -> 10s, danach 30s.
// Der Aufrufer legt Jitter drauf; diese Funktion ist deterministisch.
uint32_t reconnect_delay_ms(int attempt);

// Getrennte Fehlerklassen — WeirdIKEs Diag-Trennung wird NICHT wieder zu
// "connection failed" zusammengeworfen. Quelle: state + last_notify.
enum class VpnFailure {
    None,
    AuthenticationFailed,   // Notify 24 — Peer hat unsere Auth abgelehnt
    NoProposalChosen,       // Notify 14
    TsUnacceptable,         // Notify 38 — Child-SA/Selektoren abgelehnt
    TransportTimeout,       // FAILED ohne Auth-Notify
    UnderlayLost,           // AP5: das gewaehlte Underlay ist weggefallen
    Other,
};
const char* vpn_failure_name(VpnFailure f);

struct VpnStatus {
    bool        daemon_running = false;
    VpnState    state = VpnState::Disabled;
    VpnFailure  failure = VpnFailure::None;
    std::string raw_state;        // WeirdIKEs eigener Name (CHILD_SA_ESTABLISHED, ...)
    std::string gateway;          // "host:port" wie der Daemon ihn meldet
    std::string interface_name;
    std::string local_ts, remote_ts;
    bool        nat_t = false, nat_detected = false;
    std::string child;            // Daemon-Datenpfad: "up" | "down" (ctl_status)
    uint32_t    child_generation = 0, ike_generation = 0;
    uint32_t    last_notify = 0;
    uint32_t    uptime_s = 0;
    uint64_t    tx_packets = 0, tx_bytes = 0, rx_packets = 0, rx_bytes = 0;

    // AP5: Underlay-/Transport-Fakten. requested aus der Config, actual/
    // interface/ipv4/peer aus der SESSION (was connect() wirklich gewaehlt
    // und aufgeloest hat), ike/esp_transport GEMESSEN vom Daemon.
    std::string requested_underlay;   // underlay_name(cfg.underlay)
    std::string actual_underlay;      // "" solange keine Session
    std::string underlay_interface, underlay_ipv4;
    std::string peer_ipv4;
    std::string ike_transport;        // "udp500" | "udp4500" (Daemon)
    std::string esp_transport;        // "udp4500" (Daemon; NAT-T-only)
    std::string auth;                 // AP9: "psk" | "eap-mschapv2" (Daemon)

    // AP10: was der Daemon TUT -- die Adresse auf ipsec0, ob eine per CP
    // angefordert/zugewiesen wurde, die PFS-Gruppe, die ID-Typen in Kraft.
    std::string tunnel_ipv4;          // "" solange ipsec0 nicht konfiguriert
    bool        request_cp = false;
    std::string cp_address;           // vom Gateway zugewiesen ("" = keine)
    uint32_t    pfs_group = 0;        // 0 = kein PFS
    std::string local_id_type, remote_id_type;
    // AP11: die AUSGEHANDELTEN Suiten (Daemon aus der Engine-Diag):
    // "aes256cbc/sha256/sha256/dh14" (enc/prf/integ/dh) bzw. "aes256cbc/sha256".
    std::string ike_suite, child_suite;

    // AP7: der abgeleitete Runtime-Zustand + Reconnect-Sicht.
    VpnRuntimeState runtime = VpnRuntimeState::Disabled;
    int         reconnect_attempt = 0;    // 0 = kein Reconnect anhaengig
    bool        manual_stop = false;      // Betreiber hat gestoppt -> kein Auto-Reconnect

    // AP6: die INSTALLIERTEN Tunnelrouten, wie der Daemon sie meldet (nicht
    // die angeforderten). source: "tsr" (kryptographisch ausgehandelt) |
    // "cp" (vom Gateway geliefert). full_tunnel_refused: der Peer bot
    // 0.0.0.0/0 an, das lehnt AP6 ab.
    struct Route { std::string prefix; std::string source; std::string device; };
    std::vector<Route> routes;
    bool               full_tunnel_refused = false;
};

// AP5: die Sicht des IPsec-Service auf die Uplinks — ein Auszug, keine
// zweite Policy. select(Auto) liefert, was die bestehende Machino-Policy
// JETZT als Uplink fahren wuerde; select(Cellular) genau den einen.
struct UnderlayView {
    bool        usable = false;
    std::string kind;                 // "ethernet" | "wifi" | "cellular"
    std::string ifname, ipv4, gateway;
};
class IIpsecUplinks {
public:
    virtual ~IIpsecUplinks() = default;
    virtual bool select(Underlay wanted, UnderlayView& out, std::string& err) = 0;
};

// Parse der weirdikectl-Statuszeilen (key=value). enabled steuert nur das
// Mapping "Daemon laeuft nicht" -> Disabled|Disconnected.
VpnStatus parse_status(const std::string& text, bool daemon_running, bool enabled);

// AP12: der Test-Ping durch den Tunnel (WeirdOS "IPsec Test-Ping"). Ein
// echtes inneres ICMP-Echo an eine IPv4-Adresse HINTER dem Gateway, ueber
// das Tunnel-Interface -- nicht ueber generisches Routing. Der Beweis, dass
// ein Rechner im privaten LAN wirklich erreichbar ist.
struct PingRequest {
    std::string target;        // IPv4-Literal
    std::string ifname;        // Tunnel-Interface (ipsec0); "" = Routing entscheidet
    int         count = 1;     // 1..kPingMaxCount
    int         timeout_ms = 2000;   // je Echo
};
struct PingResult {
    std::string via;           // das Interface, ueber das gesendet wurde (vom Service gesetzt)
    int    sent = 0, received = 0;
    double rtt_min_ms = 0, rtt_avg_ms = 0, rtt_max_ms = 0;
    std::string error;         // leer = mindestens eine Antwort
};
constexpr int    kPingMaxCount     = 5;
constexpr int    kPingMinTimeoutMs = 100;
constexpr int    kPingMaxTimeoutMs = 5000;
constexpr int    kPingMaxTotalMs   = 10000;   // count * timeout: der API-Thread blockt so lange
constexpr size_t kPingHistoryMax   = 8;       // wie das WeirdOS-Dropdown: die letzten Ziele
// "a.b.c.d" in "p.q.r.s/n"? (rein; false bei Unsinn)
bool ipv4_in_prefix(const std::string& ip, const std::string& prefix);
bool ipv4_literal(const std::string& s);

// Die Naht zur Welt: Dateien liest/schreibt der Service selbst; Prozess und
// Socket gehen ueber diesen Port (Ziel: Init-Skript + /var/run/weirdike.sock;
// Hosttests: Attrappe).
class IIpsecBackend {
public:
    virtual ~IIpsecBackend() = default;
    virtual bool daemon_running() = 0;
    virtual bool start_daemon(std::string& err) = 0;   // S99weirdike start
    virtual bool stop_daemon(std::string& err) = 0;    // ctl down + S99 stop
    virtual bool ctl_status(std::string& out) = 0;     // weirdikectl status

    // AP5: DNS EINMAL vor dem Tunnel (nie spaeter ueber ipsec0) und die
    // Peer-Hostroute, die der Service besitzt: der IKE/ESP-Verkehr zum
    // Gateway bleibt IMMER auf dem Underlay, egal was spaeter an Tunnel-
    // routen verhandelt wird. gateway_ip "" = Device-Route.
    virtual bool resolve4(const std::string& host, std::string& ip_out) = 0;
    virtual bool add_peer_route(const std::string& peer_ip, const std::string& ifname,
                                const std::string& gateway_ip, std::string& err) = 0;
    virtual bool del_peer_route(const std::string& peer_ip, const std::string& ifname,
                                const std::string& gateway_ip, std::string& err) = 0;

    // AP11: Rekey auf Zuruf (weirdikectl rekey | ikerekey). Vorgabe: kann
    // das Backend nicht (Hosttests ohne Wiring). out = die ctl-Antwort.
    virtual bool rekey(bool ike_sa, std::string& out) { (void)ike_sa; out.clear(); return false; }

    // AP9: ist ein System-CA-Store in DIESEM RootFS vorhanden? (Der Daemon
    // parst ihn; machinod fragt nur, um HOST_STORE in der UI auszugrauen und
    // NIE heimlich auf NONE zurueckzufallen.) Default: nein.
    virtual bool host_store_available() { return false; }

    // AP12: ein echtes ICMP-Echo, gebunden an das Tunnel-Interface (Linux:
    // Raw-Socket + SO_BINDTODEVICE, im Prozess -- machinod forkt nicht).
    // Vorgabe: nicht verdrahtet (Hosttests ohne Netz).
    virtual bool ping(const PingRequest& req, PingResult& out) {
        (void)req; out = PingResult{}; out.error = "Ping ist in diesem Build nicht verdrahtet"; return false;
    }
};

class IpsecService {
public:
    // uplinks darf null sein (Hosttests ohne Netz, alte Verdrahtung):
    // connect() bindet dann nicht an ein Underlay (pre-AP5-Verhalten),
    // underlay=cellular wird ohne Provider ehrlich verweigert.
    IpsecService(IIpsecBackend& backend,
                 std::string machino_conf_path,
                 std::string daemon_conf_path,
                 IIpsecUplinks* uplinks = nullptr);

    // Lesen: Config OHNE Secret, plus pskSet (Daemon-Datei traegt eine
    // psk-Zeile). Fehlende Datei = Defaults.
    IpsecConfig config() const;
    bool psk_set() const;
    // AP9: Presence-Auskuenfte (nie der Wert).
    bool eap_password_set() const;
    bool ca_pem_set() const;
    bool extra_pem_set() const;
    bool host_store_available() const;   // fuer HOST_STORE-Ausgrauen in der UI

    // Speichern: validieren, BEIDE Dateien atomar schreiben (0600), Secrets
    // write-only weiterreichen. Leerer Rueckgabestring = ok.
    std::string set_config(const IpsecConfig& c, const IpsecSecrets& secrets);

    std::string connect();      // Underlay waehlen, aufloesen, Peer-Route, Daemon starten
    std::string disconnect();   // Daemon stoppen, Peer-Route entfernen
    // AP11: Child-SA (ike_sa=false) oder IKE-SA jetzt neu schluesseln --
    // Nachweis fuer PFS und die Rekey-Pfade gegen ein echtes Gateway. Leer =
    // angefordert; sonst der Grund (kein Daemon, Engine hat abgelehnt).
    std::string rekey(bool ike_sa);
    VpnStatus   status();

    // AP12: Test-Ping. Prueft Ziel und Tunnel (Child steht, Interface
    // bekannt, Ziel in einer INSTALLIERTEN Tunnelroute -- sonst ginge das
    // Paket nie durch den Tunnel), merkt sich das Ziel auf dem Geraet und
    // misst. Leerer Rueckgabestring = gemessen (auch "keine Antwort" ist ein
    // Messergebnis, es steht in out.error); sonst der Grund der Ablehnung.
    std::string ping(const std::string& target, int count, int timeout_ms, PingResult& out);
    // Die letzten Ziele, neuestes zuerst (Datei neben ipsec.conf).
    std::vector<std::string> ping_targets() const;
    std::string forget_ping_target(const std::string& target);   // leer = ok

    // AP5 §9 / AP7 §9,§10: regelmaessig aus dem Hauptthread mit der Uhr des
    // Aufrufers. AP10: der ERSTE Aufruf plant bei enabled + auto_connect den
    // Verbindungsaufbau (sofort; scheitert er, etwa weil das Mobilfunk-
    // Underlay nach dem Boot noch fehlt, greift dieselbe Backoff-Kette wie
    // beim Reconnect). Faellt das SESSION-Underlay weg, wird abgebaut und
    // Failed/UnderlayLost gemeldet (kein stiller Uplink-Wechsel). Nach einem
    // WIEDERHERSTELLBAREN Fehlschlag/Verlust plant tick() einen Reconnect mit
    // Backoff (1:sofort, 2:2s, 3:5s, 4:10s, dann 30s, +Jitter); manualStop,
    // ungueltige Config, fehlender PSK und Auth/Proposal-Fehler verhindern
    // ihn. Ein stabiler Connect setzt den Backoff zurueck.
    void tick(uint32_t now_ms);

private:
    struct Session {
        bool        active = false;
        bool        lost = false;         // Underlay weg / DPD-Verlust -> Failed
        std::string underlay_kind, ifname, ipv4, gateway_ip;
        std::string peer_ip;
        bool        peer_route = false;
    };
    void teardown_session_(bool lost);
    VpnRuntimeState derive_runtime_(const VpnStatus& s) const;
    void schedule_reconnect_(uint32_t now_ms, const IpsecConfig& c);
    std::string connect_(bool prefer_cached);
    // AP9: die PEM-Dateien liegen neben der Daemon-Datei (/etc/weirdike/).
    std::string ca_pem_path_() const;
    std::string extra_pem_path_() const;
    std::string ping_targets_path_() const;   // AP12: /etc/machino/ipsec-ping-targets
    std::string remember_ping_target_(const std::string& target);
    bool has_daemon_key_(const char* key) const;   // "key = ..." in weirdike.conf?
    static bool file_exists_(const std::string& path);

    IIpsecBackend&  backend_;
    std::string     machino_path_, daemon_path_;
    IIpsecUplinks*  uplinks_ = nullptr;
    Session         session_;

    // AP7 Reconnect/Runtime-Buchhaltung.
    bool     manual_stop_ = false;        // disconnect() setzt, connect() loescht
    int      reconnect_attempt_ = 0;
    uint32_t next_reconnect_ms_ = 0;
    bool     reconnect_scheduled_ = false;
    uint32_t last_child_gen_ = 0;
    uint32_t rekey_until_ms_ = 0;         // kurzes Fenster fuer Runtime=Rekeying
    uint32_t rng_ = 0x9e3779b9u;          // Jitter-PRNG (kein Secret)
    bool     boot_checked_ = false;       // AP10: Autostart genau einmal geprueft
    std::string cached_gateway_, cached_peer_ip_;  // AP7: DNS-Cache fuer Reconnect
};

}} // namespace machino::ipsec
