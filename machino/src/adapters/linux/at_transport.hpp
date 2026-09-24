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
#include "ports/iat_transport.hpp"
#include <string>

namespace machino { namespace linuxsys {

// Die EINZIGE echte Implementierung von IAtTransport.
//
// Dass sie das Interface auch wirklich implementiert, ist nicht
// Formsache: sonst haengt die gesamte Modemlogik an einem Testdouble und an
// nichts sonst, und die Grenze, die sie portierbar machen soll, ist eine
// Behauptung. Genau so war es im ersten Anlauf -- der Port war da, das Fake
// implementierte ihn, und dieser Adapter hatte seine eigene, unverbundene
// Antwortstruktur.
class AtTransport : public IAtTransport {
public:
    explicit AtTransport(std::string device) : device_(std::move(device)) {}

    // Schickt `cmd` mit angehaengtem CR und liest, bis eine Ergebniszeile
    // kommt oder die Frist ablaeuft.
    AtExchange command(const std::string& cmd, int timeout_ms = 3000) override;

    // Gibt es den Port? Ein stat(2), kein Kommando -- `available()` darf nach
    // seinem Vertrag nicht warten.
    bool available() const override;

    // Der Port kann sich aendern, wenn das Modem neu enumeriert: die
    // ttyUSB-Nummer ist nicht stabil, die Interfacenummer schon. Wer neu
    // sucht, setzt das Ergebnis hier ein.
    void set_device(std::string dev) { device_ = std::move(dev); }
    const std::string& device() const { return device_; }

private:
    std::string device_;
};

}} // namespace machino::linuxsys
