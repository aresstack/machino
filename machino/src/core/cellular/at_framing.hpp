// Wann ist eine AT-Antwort zu Ende?
//
// Das ist die Frage, an der ein naiver AT-Transport haengenbleibt: er liest,
// bis nichts mehr kommt, und wartet damit bei JEDEM Kommando den vollen
// Timeout ab. Bei einem Dutzend Statusabfragen sind das Sekunden, und auf
// einem Geraet mit Watchdog ist das kein Schoenheitsfehler.
//
// Ein AT-Kommando endet mit einer Ergebniszeile: "OK", "ERROR", oder eine der
// Fehlerformen mit Code. Alles davor sind Ausgabezeilen. Diese Erkennung ist
// reine Textverarbeitung -- sie gehoert in core und wird auf dem Host
// getestet, nicht am Modem.
#pragma once
#include <string>

namespace machino { namespace cellular {

enum class AtResult {
    Pending,     // noch keine Ergebniszeile gesehen
    Ok,          // "OK"
    Error,       // "ERROR", "+CME ERROR: ...", "+CMS ERROR: ...", "NO CARRIER", ...
};

// Prueft den bisher gelesenen Puffer. Die Ergebniszeile muss eine GANZE Zeile
// sein: ein "OK" mitten in einem Modellnamen beendet nichts.
AtResult at_scan(const std::string& buf);

// Die Ausgabe ohne Echo des Kommandos und ohne die Ergebniszeile, mit
// getrimmten Leerzeilen. Das ist, was ein Parser sehen will.
std::string at_payload(const std::string& buf, const std::string& command);

}} // namespace machino::cellular
