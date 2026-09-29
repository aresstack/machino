// AP11: die IKEv2-Algorithmen als Katalog -- das komplette Raster des LANCOM
// Advanced VPN Client (13 D-H-Gruppen, 8 IKE-Chiffren, 5 Hashes, 9 ESP-
// Chiffren inkl. NULL, 6 ESP-Hashes), in dessen Anzeige-Reihenfolge.
//
// Drei Wahrheiten pro Eintrag, sauber getrennt:
//   id          der Host-Name (identisch mit WeirdOS: aes256cbc, sha256, dh14)
//   iana/bits   die Kabel-Wahrheit (RFC 7296 Transform-IDs); der Daemon
//               uebersetzt die Namen in genau diese Zahlen (wd_config.c)
//   implemented ob der GEPINNTE WeirdIKE-Build sie aushandeln kann -- ein
//               Spiegel von vendor/weirdike/ike_suite.c. Nicht Implementiertes
//               bleibt konfigurierbar sichtbar (UI grau, Konsole markiert),
//               wird aber beim Speichern MIT NAMEN abgelehnt: kein stiller
//               Downgrade, kein Angebot, das der Build nicht halten kann. Der
//               Daemon prueft dieselbe Policy beim Start noch einmal gegen
//               weirdike_policy_check() -- die Engine hat das letzte Wort.
//
// Die Listen sind Allow-Listen (RFC 7296 3.3.1): ein Eintrag heisst "darf
// angeboten UND angenommen werden", keine Prioritaet. Die Engine normalisiert
// (aufsteigende IANA-ID); PFS nutzt die kleinste erlaubte D-H-Gruppe.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace machino { namespace ipsec {

enum class AlgoGroup { Dh, IkeEnc, IkeHash, EspEnc, EspHash };

struct AlgoInfo {
    const char* id;
    const char* label;          // wie im LANCOM-Dialog
    uint16_t    iana;           // ENCR/PRF/D-H-Gruppe bzw. INTEG (ESP-Hash)
    uint16_t    iana2;          // IkeHash: INTEG (iana = PRF); sonst 0
    uint16_t    bits;           // Schluessellaenge (AES), sonst 0
    bool        implemented;    // in diesem Build aushandelbar
    bool        lancom_default; // im LANCOM-DEFAULT-Profil angehakt
};

const char*     algo_group_field(AlgoGroup g);              // "ikeDh" | "ikeEnc" | ...
const char*     algo_group_title(AlgoGroup g);              // "DH-Gruppen" | ...
const AlgoInfo* algo_table(AlgoGroup g, size_t& n);
const AlgoInfo* algo_find(AlgoGroup g, const std::string& id);

// Eine Liste pruefen. Leer = ok. Sonst nennt der Text Feld und Algorithmus:
// unbekannt, in diesem Build nicht implementiert, oder leere Liste.
std::string algo_list_check(AlgoGroup g, const std::vector<std::string>& ids);

// Die kleinste erlaubte D-H-Gruppe (IANA) einer Liste -- die Gruppe, mit der
// die Engine das KE baut und die PFS benutzt. 0 bei leerer/unbekannter Liste.
uint16_t algo_smallest_dh(const std::vector<std::string>& ids);

}} // namespace machino::ipsec
