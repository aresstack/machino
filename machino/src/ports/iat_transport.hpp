// Die Grenze zwischen Modemfachlogik und Linux.
//
// Oberhalb dieser Linie gibt es kein /dev/ttyUSB, kein termios und kein USB --
// nur Kommandos und Antworten. Das ist nicht Geschmack: die gesamte
// EC200A-Logik kommt aus einem ESP32-Projekt, wo der Transport rohe
// USB-Bulk-Transfers waren. Was davon portierbar ist, ist genau das, was den
// Transport nicht kennt.
//
// Und es ist die Voraussetzung dafuer, dass diese Logik ohne Modem pruefbar
// ist: ein Skript aus aufgezeichneten Antworten beweist mehr Faelle als ein
// Geraet auf dem Tisch, weil eine SIM ohne PIN sich nicht auf Zuruf in eine
// mit PUK-Sperre verwandelt.
#pragma once
#include "core/cellular/at_framing.hpp"
#include <string>

namespace machino {

struct AtExchange {
    cellular::AtResult result = cellular::AtResult::Pending;
    std::string payload;        // Ausgabezeilen ohne Echo und Ergebniszeile
    std::string raw;
    bool        timed_out = false;
    bool        device_gone = false;

    bool ok() const { return result == cellular::AtResult::Ok; }
};

class IAtTransport {
public:
    virtual ~IAtTransport() = default;

    // Ein Kommando, eine Antwort. Blockiert hoechstens timeout_ms.
    virtual AtExchange command(const std::string& cmd, int timeout_ms = 3000) = 0;

    // Gibt es ueberhaupt einen Port? Fragt NICHT das Modem -- das waere ein
    // Kommando und damit eine Wartezeit.
    virtual bool available() const = 0;
};

} // namespace machino
