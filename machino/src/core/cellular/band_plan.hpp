// LTE-Baender und Netzmodus des EC200A-EU: Tabelle, Profile, Bitmasken.
//
// Portiert aus esp32-modem-host/ec200a_modem.cpp (LTE_BANDS, modemProfileLteMask,
// modemNwScanMode) und dort am echten Modem belegt. Zwei Eigenheiten des
// EC200A, die vom Quectel-Standardhandbuch abweichen und ohne die jeder
// Nachbau am Geraet scheitert:
//
//   AT+QCFG="band",<GSM>,<LTE>     BARE-HEX ohne 0x, und nur ZWEI Parameter.
//                                  Ein 0x-Praefix ODER ein dritter Parameter
//                                  (TDS) ergibt +CME ERROR: 50.
//   GSM = "d3"                     behaelt den GSM-Wert; die LTE-Maske ist
//                                  bit(n-1) = Band n.
//
// Der Wert ist im Modem PERSISTENT. Angewendet wird er ueber einen RF-Zyklus
// (AT+CFUN=0 / AT+CFUN=1), NICHT ueber AT+CFUN=1,1: RF aus und wieder an
// genuegt fuer eine neue Zellsuche und loest keine USB-Re-Enumeration aus.
//
// Die Tabelle ist der Bandsatz der EU-Variante (FDD B1/B3/B5/B7/B8/B20/B28,
// TDD B38/B40/B41). Das Modem meldet seine Bandfaehigkeit nicht per AT --
// AT+QCFG="band" liefert nur die aktuelle Maske --, deshalb steht die Liste
// hier und nicht in einer Abfrage. Eine andere Variante (CN/AU) lehnt fremde
// Bits beim Schreiben ab, und genau das wird dann gemeldet.
//
// Alles hier ist rein: keine I/O, kein Zustand. Was das Modem tut, steht in
// radio_tuner.
#pragma once
#include "core/cellular/at_parse.hpp"
#include <cstdint>
#include <string>
#include <vector>

namespace machino { namespace cellular {

struct LteBand {
    int  num;    // Bandnummer, z.B. 20
    int  mhz;    // nominale Mittenfrequenz, fuer die Anzeige
    int  bit;    // Position in der QCFG-Maske: num - 1
    bool tdd;
};

// Die Baender, die dieses Modul kann -- nicht alle, die es gibt.
const std::vector<LteBand>& lte_bands();
const LteBand* lte_band_by_number(int num);

// ---------------------------------------------------------------- Netzmodus

// AT+QCFG="nwscanmode",<n>,1. Das EC200A kann KEIN 3G (nur 2G und 4G), deshalb
// gibt es hier kein WCDMA: ein Wert 2 waere ein Modus, den das Geraet nicht
// hat, und ein Firmwarestand lehnt ihn mit CME ERROR 50 ab.
enum class NetMode { Auto, LteOnly, GsmOnly };

const char* net_mode_name(NetMode m);                 // "auto" | "lte" | "gsm"
bool        net_mode_parse(const std::string& s, NetMode& out);
int         net_mode_nwscanmode(NetMode m);           // 0 | 3 | 1
bool        net_mode_from_nwscanmode(int v, NetMode& out);

// ------------------------------------------------------------------ Profile

// Die Profile aus dem Referenzprojekt, wortgleich:
//   Auto    alle unterstuetzten Baender
//   Mid     1-2 GHz, niedrige Latenz: B3 (1800)
//   Low     unter 1 GHz, maximale Reichweite: B20, B8, B28, B5
//   Custom  eine eigene Maske
enum class BandProfile { Auto, Mid, Low, Custom };

const char* band_profile_name(BandProfile p);         // "auto" | "mid" | "low" | "custom"
bool        band_profile_parse(const std::string& s, BandProfile& out);

// ------------------------------------------------------------------- Masken

uint64_t all_supported_mask();

// Die Maske, die fuer ein Profil geschrieben wird. Bei Custom die eigene; ist
// sie leer, faellt das NICHT auf "alle" zurueck, sondern bleibt 0 -- der
// Aufrufer muss das als "nichts gewaehlt" ablehnen, statt still alles zu
// erlauben.
uint64_t profile_mask(BandProfile p, uint64_t custom_mask);

// Bandnummern <-> Maske. Nur Baender aus der Tabelle; eine unbekannte Nummer
// ist ein Fehler und wird in `bad` genannt.
bool     mask_from_bands(const std::vector<int>& bands, uint64_t& out, int& bad);
std::vector<int> bands_from_mask(uint64_t mask);

// So wie das Modem es will: Hex ohne 0x, ohne fuehrende Nullen, klein.
std::string mask_hex(uint64_t mask);
// Nimmt beides: "1a0080800d5" und "0x1a0080800d5". Leer oder Nicht-Hex = false.
bool        parse_mask_hex(const std::string& s, uint64_t& out);

// +QCFG: "band",<gsm>,<lte>[,<tds>] -- die Rueckmeldung des Modems.
//
// Beide Werte als Hex gelesen, mit oder ohne 0x: das Modem schreibt sie so,
// wie es sie nimmt. Ein Feld, das keine Hexzahl ist, bleibt ABWESEND -- eine
// nicht gelesene Maske darf nicht als 0 durchgehen, weil 0 "kein Band" hiesse
// und ein Vergleich damit sofort einen Schreibvorgang ausloeste.
struct QcfgBand {
    Maybe<uint64_t> gsm;
    Maybe<uint64_t> lte;
};
QcfgBand parse_qcfg_band(const std::string& raw);

}} // namespace machino::cellular
