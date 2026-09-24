// Port: der PPP-Datenpfad, so weit er plattformabhaengig ist.
//
// WAS HIER *NICHT* STEHT, IST DIE HAUPTSACHE
//
// Kein PPP-Protokoll. Unter Linux gibt es pppd, und einen zweiten LCP/IPCP/PAP-
// Stack danebenzustellen waere Arbeit an einem geloesten Problem -- mit dem
// Unterschied, dass der zweite Stack niemand ausser uns getestet hat. Die
// Referenzimplementierung hat ihren eigenen gebraucht, weil auf einem ESP32
// lwIP mit PPPoS die einzige Moeglichkeit war. Diese Begruendung gilt hier
// nicht.
//
// Kein AT-Kommando. SIM, PIN, Registrierung, APN und PDP-Kontext gehoeren der
// gemeinsamen Control Plane (CellularService, SimManager, PppLink); ECM und
// PPP unterscheiden sich NUR im Datenlink. Ein Backend, das selbst AT spricht,
// waere eine zweite Control Plane und damit ein zweiter Ort, an dem eine PIN
// verbraucht werden kann.
//
// Kein Routing und kein DNS. Beides entscheidet core/net/route_plan fuer alle
// Uplinks zusammen. Das Backend meldet, was der Link bekommen hat.
#pragma once
#include "core/cellular/cellular_config.hpp"
#include "core/result.hpp"
#include "ports/iecm_backend.hpp"        // LinkAddress
#include <string>

namespace machino { namespace cellular {

// Alles, was pppd fuer diesen Anruf braucht.
//
// Ein Struct und keine Kommandozeile, und das ist der Punkt: `password` darf
// in keinem argv stehen. Auf dieser Kamera liest jeder Prozess /proc, und ein
// APN-Passwort in `ps` ist ein Passwort, das jeder hat. Das Backend legt es in
// eine Datei mit 0600.
struct PppRequest {
    std::string tty;            // der Modem-Port, dynamisch ermittelt
    int         baud = 115200;  // bei USB-CDC belanglos, pppd will trotzdem eine Zahl
    std::string apn;
    std::string dial;           // "*99***1#"
    PdpType     pdp = PdpType::Ipv4;
    AuthMode    auth = AuthMode::None;
    std::string username;
    std::string password;       // GEHEIM: nie argv, nie Log, nie Antwort
};

// Warum pppd aufgehoert hat. Uebersetzt aus seinem Exit-Code, weil "pppd ist
// mit 19 beendet" niemandem hilft und "Authentifizierung abgelehnt" schon.
enum class PppExit {
    None = 0,        // laeuft noch oder wurde nie gestartet
    Ok,              // planmaessig beendet
    NoDevice,        // Modem-Port weg
    DialFailed,      // kein CONNECT
    AuthFailed,
    NegotiationFailed,
    LinkTerminated,  // Gegenstelle hat aufgelegt
    Other,
};

const char* ppp_exit_name(PppExit e);

struct PppStatus {
    bool        running = false;   // pppd laeuft
    std::string ifname;            // "" solange kein pppX existiert
    LinkAddress address;           // gefuellt, sobald ip-up gelaufen ist
    PppExit     exit = PppExit::None;
    std::string detail;            // Klartext, ohne Geheimnisse
};

class IPppBackend {
public:
    virtual ~IPppBackend() = default;

    // pppd starten. Der Aufruf kehrt zurueck, sobald der Wunsch abgesetzt ist
    // -- ob der Anruf zustande kommt, sagt spaeter status().
    virtual Result start(const PppRequest& req) = 0;

    // Abbauen. Muss mehrfach aufrufbar sein, ohne dass beim zweiten Mal etwas
    // passiert.
    virtual Result stop() = 0;

    // Was gerade ist. False heisst "konnte nicht nachsehen" und NICHT "nichts
    // laeuft" -- die beiden zu verwechseln hiesse, einen laufenden pppd fuer
    // tot zu erklaeren und einen zweiten danebenzustellen.
    virtual bool status(PppStatus& out) const = 0;
};

}} // namespace machino::cellular
