// Die persistente Mobilfunkkonfiguration.
//
// `enabled` ist hier NICHT der gemeinsame USB-Selektor. Der kommt in AP-M6 und
// heisst dann usb.function = off|wifi|cellular, weil es genau einen Port gibt
// und zwei unabhaengige Haken eine Falle waeren. Bis dahin beschreibt dieses
// Feld nur die Absicht "Mobilfunk soll benutzt werden" und schaltet von sich
// aus keinen Bootpfad -- zwei widersprechende Bootpfade einzubauen und sie
// spaeter wieder auseinanderzunehmen waere mehr Arbeit als sie jetzt nicht zu
// bauen.
//
// Providerwerte stehen NICHT im Code. Die beiden bekannten Konfigurationen aus
// der Vorarbeit sind Vorschlaege, die jemand auswaehlen kann:
//
//     Telekom   internet.t-d1.de   PDP IP, keine Auth
//               -- die Standard-APNs geben nur CGNAT (10.x/100.64.x)
//     o2        netpublic          PDP IP
//               -- "internet" gibt dort ebenfalls nur 10.x
//
// Beide stammen aus esp32-modem-host (TELEKOM-PUBLIC-IPV4.md bzw.
// MODEM_APN_DEFAULT) und sind dort an echten SIM-Karten belegt.
#pragma once
#include <string>
#include <vector>

namespace machino { namespace cellular {

enum class PdpType { Ipv4, Ipv4v6 };
enum class AuthMode { None, Pap, Chap };

const char* pdp_type_name(PdpType t);
bool        pdp_type_parse(const std::string& s, PdpType& out);
const char* auth_mode_name(AuthMode a);
bool        auth_mode_parse(const std::string& s, AuthMode& out);

struct CellularConfig {
    bool        enabled = false;
    std::string apn;
    PdpType     pdp = PdpType::Ipv4;
    AuthMode    auth = AuthMode::None;
    std::string username;
    std::string password;      // GEHEIM: nie in Antworten, nie in Logs
    bool        auto_connect = false;
    std::string sim_pin;       // GEHEIM, dito
};

struct ApnPreset {
    const char* id;
    const char* label;
    const char* apn;
    PdpType     pdp;
    AuthMode    auth;
    const char* note;
};

// Bekannte, an echter Hardware belegte Konfigurationen. Vorschlaege, keine
// Automatik: welcher Anbieter in der Kamera steckt, weiss machino nicht.
const std::vector<ApnPreset>& apn_presets();

}} // namespace machino::cellular
