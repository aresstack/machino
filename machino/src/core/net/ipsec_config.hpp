// IPsec-Konfiguration (AP3, Feature 2): typisiert in machinod, ausgefuehrt
// von weirdiked. machinod linkt KEIN WeirdIKE — die Prozessgrenze aus
// PLATFORM.md bleibt. Dieses Modul besitzt zwei Dateien:
//
//   /etc/machino/ipsec.conf      die machino-Wahrheit (typisiert, OHNE Secret)
//   /etc/weirdike/weirdike.conf  daraus GENERIERT, traegt zusaetzlich den PSK
//
// Der PSK ist write-only: er steht nur in der generierten Daemon-Datei
// (0600); ein Speichern ohne neuen PSK traegt den vorhandenen unveraendert
// weiter. Kein API-Weg gibt ihn je zurueck.
//
// Algorithmen: fuer AP3 ist AUSSCHLIESSLICH die in AP2 gegen strongSwan
// bewiesene Suite freigeschaltet (aes256cbc / sha256 / dh14). Alles andere
// wird MIT NAMEN abgelehnt — kein stilles Ignorieren, kein Downgrade. Die
// IDs sind die fachlichen aus der P4-Referenz (ipsec_crypto_caps).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace machino { namespace ipsec {

enum class Underlay { Auto, Ethernet, Wifi, Cellular };
const char* underlay_name(Underlay u);
bool underlay_from_name(const std::string& s, Underlay& out);

// AP9: Auth-Modus + Trust-Modell. Namen matchen 1:1 die WeirdIKE-Typen
// (weirdike_trust_mode_t); die UI zeigt lesbare Labels, intern bleibt es
// exakt dieses Mapping.
enum class Auth { Psk, EapMschapv2 };
const char* auth_name(Auth a);
bool auth_from_name(const std::string& s, Auth& out);

enum class TrustMode { AnchorPem, HostStore, HostStorePlusPem, None };
const char* trust_mode_name(TrustMode t);
bool trust_mode_from_name(const std::string& s, TrustMode& out);

struct IpsecConfig {
    bool        enabled = false;
    std::string gateway;              // Hostname oder IPv4-Literal
    uint16_t    port = 500;
    Underlay    underlay = Underlay::Auto;   // AP3: persistiert; Bindung ist AP5
    std::string local_id;             // FQDN-artig (weirdiked: WEIRDIKE_ID_FQDN)
    std::string remote_id;
    std::string local_subnet;         // CIDR "a.b.c.d/n" — die innere Adresse
    std::string remote_subnet;        // CIDR — genau EIN Split-Netz (AP4-Scope)
    bool        nat_t = true;
    int         dpd_interval_s = 30;
    uint32_t    ike_lifetime_s = 0;   // 0 = weirdiked-Default
    uint32_t    child_lifetime_s = 0;

    // AP9: Authentifizierung. PSK (Default) oder EAP-MSCHAPv2 (User+Passwort;
    // der Server weist sich per Zertifikat aus). eap_password ist write-only
    // wie der PSK (nie in to_machino_conf, nie zurueck). trust_mode steuert
    // die Zertifikatspruefung; ca_pem/extra_pem sind PEM-Inhalte (oeffentlich,
    // aber in Machinos eigener Config, 0600).
    Auth        auth = Auth::Psk;
    std::string eap_user;             // Identity/Username (kein Secret)
    TrustMode   trust_mode = TrustMode::HostStore;

    // Geschlossene Mengen; AP3 erlaubt exakt die AP2-Suite.
    std::vector<std::string> ike_enc  {"aes256cbc"};
    std::vector<std::string> ike_hash {"sha256"};
    std::vector<std::string> ike_dh   {"dh14"};
    std::vector<std::string> esp_enc  {"aes256cbc"};
    std::vector<std::string> esp_hash {"sha256"};
};

// Validierung: leerer Fehlerstring = ok; sonst benennt er das ERSTE Problem
// konkret (Feld + Wert). Ein enabled=true ohne Gateway/PSK-Vorhandensein wird
// hier NICHT geprueft (pskSet kennt nur der Service).
std::string validate(const IpsecConfig& c);

// PSK-Regeln (leer = ok, "keinen neuen setzen"): 1..64 Bytes druckbares
// ASCII, keine Zeilenumbrueche (Config-Injection!), keine Randleerzeichen
// (der Daemon-Parser trimmt). Die Meldung nennt nie den Wert. AP9: dieselben
// Regeln gelten fuer das EAP-Passwort.
std::string psk_check(const std::string& psk);

// AP9: write-only-Secrets fuer set_config. Jedes leere Feld = "unveraendert
// lassen" (der vorhandene Wert wird weitergetragen). Kein Feld erscheint je
// in einer GET-Antwort oder Fehlermeldung.
struct IpsecSecrets {
    std::string psk;            // PSK-Auth
    std::string eap_password;   // EAP-Auth
    std::string ca_pem;         // Trust-Anchor / Server-CA (oeffentlich, aber 0600)
    std::string extra_pem;      // zusaetzliches Chain-Material
};

// machino-Datei (ohne Secret): serialisieren/parsen, key=value-Zeilen.
std::string to_machino_conf(const IpsecConfig& c);
bool from_machino_conf(const std::string& text, IpsecConfig& out, std::string& err);

// AP5: Session-Werte, die connect() fuer GENAU diese Sitzung in die
// Daemon-Datei schreibt: das VOR dem Tunnel aufgeloeste Gateway (nie DNS
// ueber ipsec0; Rekey nutzt dieselbe Adresse) und die konkrete Underlay-
// Bindung. Alles leer = keine Session-Pinnung (pre-AP5-Verhalten).
struct SessionNet {
    std::string gateway_ip;   // aufgeloestes IPv4-Literal
    std::string bind_ip;      // konkrete Underlay-IPv4
    std::string bind_dev;     // Underlay-Interface (SO_BINDTODEVICE)
};

// Daemon-Datei generieren. Das jeweils passende Secret (psk ODER eap_password
// je nach c.auth) wird write-only behandelt: leer = den alten Zeilenwert aus
// old_daemon_conf weitertragen. *cred_present meldet, ob am Ende das fuer den
// Auth-Modus noetige Credential vorhanden ist (Aufrufer -> enabled-Gate).
// ca_pem_file/extra_pem_file: Pfade der vom Service geschriebenen PEM-Dateien
// (leer = keine); nur bei EAP emittiert.
std::string to_weirdike_conf(const IpsecConfig& c, const IpsecSecrets& secrets,
                             const std::string& old_daemon_conf, bool* cred_present,
                             const SessionNet* net = nullptr,
                             const std::string& ca_pem_file = std::string(),
                             const std::string& extra_pem_file = std::string());

// Atomar (tmp + rename), Modus 0600. Plattformneutral genug fuer Hosttests.
bool write_atomic_0600(const std::string& path, const std::string& content,
                       std::string& err);

}} // namespace machino::ipsec
