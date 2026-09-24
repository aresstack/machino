// Was machino ueber das Mobilfunkmodem weiss.
//
// Nur Felder, die aus der vorhandenen EC200A-Logik wirklich bestimmt werden
// koennen. Keine QMI-, HiSilicon- oder 5G-Abstraktionen: das Geraet ist ein
// EC200A-EU, LTE Cat-4, und ein Modell, das mehr verspricht, muesste den Rest
// erfinden.
//
// Jeder Zahlenwert ist ein Maybe. Ein fehlendes RSRP ist nicht -0 dBm und ein
// fehlender Kanal nicht 0 -- beides waeren Messwerte, und in einer Anzeige
// sieht man ihnen nicht an, dass sie erfunden sind.
#pragma once
#include "core/cellular/at_parse.hpp"
#include <cstdint>
#include <string>

namespace machino { namespace cellular {

struct CellularStatus {
    // Erreichbarkeit. `present` heisst: es gibt einen AT-Port. `responsive`
    // heisst: darauf hat jemand geantwortet. Der Unterschied ist die halbe
    // Diagnose -- ein Modem, das enumeriert aber schweigt, ist ein anderer
    // Fehler als eines, das gar nicht da ist.
    bool present = false;
    bool responsive = false;

    ModemIdentity identity;
    std::string   imei;

    SimState    sim = SimState::Unknown;
    std::string sim_detail;
    std::string iccid;
    std::string imsi;

    RegState    registration = RegState::Unknown;
    std::string operator_name;
    std::string operator_code;

    std::string rat;            // "FDD LTE" aus QNWINFO
    ServingCell cell;           // Band, EARFCN, RSRP/RSRQ/RSSI/SINR, TAC ...
    SignalInfo  signal;         // CSQ und das daraus abgeleitete RSSI

    PdpAddress  pdp;            // Adresse des Kontexts, falls aktiv

    std::string last_error;     // letzter Klartextgrund, ohne Geheimnisse
    uint64_t    last_update_ms = 0;
};

}} // namespace machino::cellular
