// Ein AT-Kommando ueber /dev/ttyUSB* schicken und die Antwort holen.
//
// Bewusst klein. Keine Zustandsmaschine, keine Parser, keine Wiederholungen --
// das ist AP-M3 und kommt aus der Referenzimplementierung
// (esp32-modem-host/ec200a_modem.*), nicht aus einer Neuerfindung hier.
//
// Der Port wird fuer die Dauer eines Kommandos geoeffnet und wieder
// geschlossen. Das ist fuer Statusabfragen genau richtig und fuer einen
// dauerhaft mitlesenden URC-Kanal genau falsch; wenn AP-M3 unaufgeforderte
// Meldungen braucht, kommt dafuer ein offen gehaltener Kanal dazu, und dann
// muss geklaert werden, wer den Port besitzt. Heute besitzt ihn niemand
// dauerhaft, und das ist die einfachere Wahrheit.
//
// Ein verschwundenes Modem darf nicht haengen: jeder Schritt hat eine Frist,
// und ein Lesefehler ist ein Ergebnis, kein Grund zu warten.
#pragma once
#include "core/cellular/at_framing.hpp"
#include "core/result.hpp"
#include <string>

namespace machino { namespace linuxsys {

struct AtReply {
    cellular::AtResult result = cellular::AtResult::Pending;
    std::string payload;      // Ausgabezeilen ohne Echo und Ergebniszeile
    std::string raw;          // alles, wie gelesen -- fuer die Diagnose
    bool        timed_out = false;
    bool        device_gone = false;   // Port verschwunden / EIO
};

class AtTransport {
public:
    explicit AtTransport(std::string device) : device_(std::move(device)) {}

    // Schickt `cmd` mit angehaengtem CR und liest, bis eine Ergebniszeile
    // kommt oder die Frist ablaeuft.
    Result send(const std::string& cmd, AtReply& out, int timeout_ms = 3000);

    const std::string& device() const { return device_; }

private:
    std::string device_;
};

}} // namespace machino::linuxsys
