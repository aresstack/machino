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

    // Betriebsart des Modem-NIC (AT+QCFG="nat"). Entscheidet, WOHER die
    // Adresse kommt, und das ist keine Feinheit:
    //
    //   true  = NIC     das Modem reicht die oeffentliche Adresse direkt
    //                   durch und beantwortet KEIN DHCP; alles kommt aus
    //                   AT+CGCONTRDP
    //   false = Routing das Modem NATet, DHCP liefert 192.168.43.x, von
    //                   aussen ist nichts erreichbar
    //
    // Default true, weil das Referenzprojekt ihn so hat (`modemNatMode = "nic"`
    // in ec200a_modem.cpp). Nicht hier neu entschieden.
    //
    // Die Einstellung ist im Modem PERSISTENT und ein Wechsel braucht einen
    // Modem-Neustart -- sie wird deshalb hoechstens einmal je Lebenszyklus
    // durchgesetzt.
    bool        nic_mode = true;

    // Welcher Datenlink: ECM oder PPP.
    //
    // Der Benutzer waehlt "Mobilfunk" -- das hier ist die Auspraegung darunter
    // und gehoert ausdruecklich NICHT in die Uplink-Auswahl. `ppp0` taucht in
    // keiner Preference-Liste auf.
    //
    // Default ECM, und das bleibt so. PPP ist die Alternative fuer den Fall,
    // dass ECM auf einem Modem oder in einem Netz nicht geht -- ein
    // Kompatibilitaetsweg, kein gleichwertiger zweiter Hauptpfad. Die
    // Referenzimplementierung sagt dasselbe: `modemDataMode = "ecm"` ist dort
    // als "Standard/Produktion" markiert, PPP als "Kompatibilitaet".
    //
    // Es gibt KEINEN automatischen Wechsel. Scheitert ECM, bleibt es bei ECM
    // und sagt warum. Ein stiller Fallback haette zur Folge, dass eine Kamera
    // auf einem Weg laeuft, den niemand gewaehlt hat, und dass der Fehler im
    // gewaehlten Weg nie auffaellt.
    //
    // Wirkt beim naechsten Neustart: die beiden Wege brauchen verschiedene
    // Kernelmodule, und die tauscht machino nicht bei laufender IMP-Pipeline.
    std::string data_link = "ecm";      // "ecm" | "ppp"

    // Die Einwahlnummer fuer PPP. *99***1# ist die GPRS/LTE-Standardnummer und
    // der Default der Referenz (MODEM_DIAL_DEFAULT); konfigurierbar, weil ein
    // paar Netze *99# oder eine kontextbezogene Variante wollen.
    std::string dial = "*99***1#";
};

struct ApnPreset {
    const char* id;
    const char* label;
    const char* apn;
    PdpType     pdp;
    AuthMode    auth;
    const char* user;   // leer, ausser der Anbieter verlangt Zugangsdaten (Telekom: t-mobile)
    const char* pass;   // dito (Telekom: tm) -- oeffentlich bekannt, kein Geheimnis
    const char* note;
};

// Bekannte, an echter Hardware belegte Konfigurationen. Vorschlaege, keine
// Automatik: welcher Anbieter in der Kamera steckt, weiss machino nicht.
const std::vector<ApnPreset>& apn_presets();

}} // namespace machino::cellular
