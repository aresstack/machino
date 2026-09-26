// IpsecService (AP3): die machinod-Seite des VPN — Konfiguration, Persistenz,
// Lifecycle, Status. Er spricht mit weirdiked ueber die PROZESSGRENZE
// (PLATFORM.md): Konfig-Dateien, Init-Skript, Control-Socket. Kein
// WeirdIKE-Symbol in machinod; die IKEv2-Wahrheit kommt ausschliesslich aus
// weirdikeds Status (der sie 1:1 aus weirdike_state/get_diag zieht) — nie
// aus "Socket offen" oder eigener Fantasie.
#pragma once
#include "core/net/ipsec_config.hpp"

#include <string>

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

// Getrennte Fehlerklassen — WeirdIKEs Diag-Trennung wird NICHT wieder zu
// "connection failed" zusammengeworfen. Quelle: state + last_notify.
enum class VpnFailure {
    None,
    AuthenticationFailed,   // Notify 24 — Peer hat unsere Auth abgelehnt
    NoProposalChosen,       // Notify 14
    TsUnacceptable,         // Notify 38 — Child-SA/Selektoren abgelehnt
    TransportTimeout,       // FAILED ohne Auth-Notify
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
};

// Parse der weirdikectl-Statuszeilen (key=value). enabled steuert nur das
// Mapping "Daemon laeuft nicht" -> Disabled|Disconnected.
VpnStatus parse_status(const std::string& text, bool daemon_running, bool enabled);

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
};

class IpsecService {
public:
    IpsecService(IIpsecBackend& backend,
                 std::string machino_conf_path,
                 std::string daemon_conf_path);

    // Lesen: Config OHNE Secret, plus pskSet (Daemon-Datei traegt eine
    // psk-Zeile). Fehlende Datei = Defaults.
    IpsecConfig config() const;
    bool psk_set() const;

    // Speichern: validieren, BEIDE Dateien atomar schreiben (0600), PSK
    // write-only weiterreichen. Leerer Rueckgabestring = ok.
    std::string set_config(const IpsecConfig& c, const std::string& psk_or_empty);

    std::string connect();      // Voraussetzungen pruefen, Daemon starten
    std::string disconnect();   // Daemon stoppen (ctl down + stop)
    VpnStatus   status();

private:
    IIpsecBackend& backend_;
    std::string machino_path_, daemon_path_;
};

}} // namespace machino::ipsec
