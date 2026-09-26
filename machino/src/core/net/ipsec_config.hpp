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

// machino-Datei (ohne Secret): serialisieren/parsen, key=value-Zeilen.
std::string to_machino_conf(const IpsecConfig& c);
bool from_machino_conf(const std::string& text, IpsecConfig& out, std::string& err);

// Daemon-Datei generieren. psk leer = vorhandenen psk-Wert aus old_daemon_conf
// uebernehmen (write-only-Semantik). Liefert false, wenn am Ende KEIN PSK da
// waere (Speichern ok, aber der Aufrufer soll pskSet=false wissen).
std::string to_weirdike_conf(const IpsecConfig& c, const std::string& psk,
                             const std::string& old_daemon_conf, bool* psk_present);

// Atomar (tmp + rename), Modus 0600. Plattformneutral genug fuer Hosttests.
bool write_atomic_0600(const std::string& path, const std::string& content,
                       std::string& err);

}} // namespace machino::ipsec
