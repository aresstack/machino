// Die AT-Parser, portiert aus dem ESP32-Projekt.
//
// Referenz: esp32-modem-host/esp32-modem-host.ino, handleModemJson() und die
// Helfer atExtract/csvField/firstNumericLine, plus die LTE_BANDS-Tabelle aus
// ec200a_modem.cpp. Die Feldindizes sind nicht hergeleitet, sondern von dort
// uebernommen -- sie stammen aus Messungen an einem echten EC200A und nicht
// aus einem Datenblatt.
//
// Alles hier ist rein: Text rein, Struktur raus. Kein I/O, kein Zustand. Genau
// deshalb ist es host-testbar, und genau deshalb kann ein Fixture aus einer
// aufgezeichneten Antwort mehr Faelle abdecken als ein Modem auf dem Tisch --
// eine SIM ohne PIN laesst sich nicht auf Zuruf in eine mit PUK-Sperre
// verwandeln.
//
// EIN Grundsatz zieht sich durch: ein Wert, der nicht gelesen werden konnte,
// ist ABWESEND. Nicht 0, nicht "", nicht -1 mit Sonderbedeutung. Ein RSRP von
// 0 dBm waere ein sensationeller Messwert; als Platzhalter fuer "keine Antwort"
// ist es eine Luege, die in einer Anzeige landet.
#pragma once
#include <string>
#include <vector>

namespace machino { namespace cellular {

// Ein Messwert, der auch fehlen darf.
template <typename T>
struct Maybe {
    bool has = false;
    T    value{};

    Maybe() = default;
    explicit Maybe(T v) : has(true), value(v) {}
    T value_or(T fallback) const { return has ? value : fallback; }
};

using MaybeInt = Maybe<int>;

// ---------------------------------------------------------------- Helfer ---

// Der Text NACH `prefix` bis zum Zeilenende, getrimmt. Leer, wenn der Prefix
// nicht vorkommt. (ESP32: atExtract)
std::string at_extract(const std::string& raw, const std::string& prefix);

// CSV-Feld `idx`, wobei Anfuehrungszeichen Kommas schuetzen. Betreibernamen
// enthalten Kommas, und QENG liefert gemischt zitierte und nackte Felder.
// (ESP32: csvField)
std::string csv_field(const std::string& line, int idx);

// Die erste Zeile, die nur aus Ziffern besteht und mindestens sechs lang ist.
// IMEI und IMSI kommen ohne Prefix zurueck, und vor ihnen kann das Echo
// stehen. (ESP32: firstNumericLine)
std::string first_numeric_line(const std::string& raw);

// Nominale Mittenfrequenz eines LTE-Bandes in MHz. Tabelle aus LTE_BANDS
// (ec200a_modem.cpp) -- die Baender, die dieses Modul unterstuetzt, nicht alle
// die es gibt. 0 = unbekannt.
int lte_band_mhz(int band);

// -------------------------------------------------------------- Ergebnisse --

struct ModemIdentity {
    std::string manufacturer;   // "Quectel"
    std::string model;          // "EC200A"
    std::string firmware;       // "EC200AEUV1HAR02A07M16"
};

// ATI liefert mehrere Zeilen: Hersteller, Modell, "Revision: <firmware>".
ModemIdentity parse_ati(const std::string& raw);

enum class SimState {
    Unknown,
    Ready,
    PinRequired,
    PukRequired,
    NotInserted,
    Error,
};

const char* sim_state_name(SimState s);

// Aus der Antwort auf AT+CPIN?. Ein CME ERROR heisst hier meistens "keine
// SIM"; welcher Code genau, steht nicht zuverlaessig fest, deshalb wird der
// Fall als NotInserted gefuehrt und der Rohtext bleibt erhalten.
SimState parse_cpin(const std::string& raw);

enum class RegState {
    Unknown,
    NotRegistered,      // 0
    RegisteredHome,     // 1
    Searching,          // 2
    Denied,             // 3
    RegisteredRoaming,  // 5
};

const char* reg_state_name(RegState s);
bool reg_is_registered(RegState s);

// +CEREG: <n>,<stat>[,...]. Ein blosses true/false waere hier zu wenig: "Netz
// wird gesucht" und "Registrierung verweigert" verlangen verschiedene
// Reaktionen, und der spaetere Verbindungsaufbau muss sie unterscheiden.
// Wert 4 ("unknown") des Standards wird zu Unknown.
RegState parse_cereg(const std::string& raw);

struct SignalInfo {
    MaybeInt csq;        // 0..31, 99 = unbekannt -> abwesend
    MaybeInt rssi_dbm;   // aus csq abgeleitet: -113 + 2*csq
};

// +CSQ: <rssi>,<ber>. Der Standardwert 99 bedeutet "nicht bekannt" und wird
// deshalb NICHT als Zahl weitergereicht.
SignalInfo parse_csq(const std::string& raw);

struct OperatorInfo {
    std::string name;    // Feld 2
    MaybeInt    act;     // Feld 3, Zugangstechnologie
};

// +COPS: <mode>,<format>,<oper>[,<act>]
OperatorInfo parse_cops(const std::string& raw);

// +QNWINFO: <act>,<oper>,<band>,<channel>
struct NwInfo {
    std::string rat;       // "FDD LTE"
    std::string oper_code; // "26201"
    std::string band;      // "LTE BAND 3"
    MaybeInt    channel;
};
NwInfo parse_qnwinfo(const std::string& raw);

// +QENG: "servingcell",<state>,<rat>,<duplex>,<mcc>,<mnc>,<cellid>,<pci>,
//        <earfcn>,<band>,...,<tac>,<rsrp>,<rsrq>,<rssi>,<sinr>
//
// Die Indizes stammen aus dem Referenzprojekt (Kommentar in handleModemJson):
// 4=MCC 5=MNC 6=CellID 7=PCI 8=EARFCN 9=Band 12=TAC 13=RSRP 14=RSRQ 15=RSSI
// 16=SINR. Fehlende oder nicht numerische Felder bleiben abwesend -- QENG
// liefert bei schwachem Empfang und im Idle durchaus Luecken und "-".
struct ServingCell {
    std::string state;       // "NOCONN" / "CONNECT"
    std::string rat;         // "LTE"
    std::string mcc, mnc;
    std::string cell_id, tac;
    MaybeInt pci, earfcn, band, band_mhz;
    MaybeInt rsrp, rsrq, rssi, sinr;
};
ServingCell parse_qeng_servingcell(const std::string& raw);

// +CGPADDR: <cid>,<addr>[,<addr2>]
struct PdpAddress {
    std::string ipv4;
    std::string ipv6;
};
PdpAddress parse_cgpaddr(const std::string& raw);

}} // namespace machino::cellular
