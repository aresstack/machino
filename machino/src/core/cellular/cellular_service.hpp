// Die Control Plane des EC200A: fragen, nicht stellen.
//
// Dieser Dienst LIEST den Zustand des Modems und haelt ihn vor. Er aendert
// nichts Dauerhaftes -- kein AT+QCFG, kein AT+CFUN=1,1, kein AT+QNETDEVCTL,
// kein ATD. Diese Kommandos sind teils persistent im Modem und loesen teils
// eine Re-Enumeration aus; sie gehoeren zum Verbindungsaufbau und damit in
// einen eigenen Schritt. Ein Statusdienst, der beim Start die USB-Komposition
// des Geraets umstellt, waere eine unangenehme Ueberraschung.
//
// Die einzige Ausnahme ist AT+CPIN=, und die ist genau deshalb in SimManager
// eingesperrt, wo der Schutz gegen Wiederholung sitzt.
//
// Portiert aus esp32-modem-host/ec200a_modem.cpp (RF-Snapshot, Identitaet,
// Registrierung) und modem_sim.cpp (SIM). Die Reihenfolge der Abfragen ist von
// dort uebernommen: erst wer bist du, dann ist die SIM bereit, dann bist du im
// Netz, dann wie steht es um den Funk. Frueher abzubrechen spart Zeit und
// verhindert Werte, die ohne den Schritt davor nichts bedeuten.
#pragma once
#include "core/cellular/cellular_config.hpp"
#include "core/cellular/cellular_status.hpp"
#include "core/cellular/sim_manager.hpp"
#include "ports/iat_transport.hpp"
#include <cstdint>
#include <functional>

namespace machino { namespace cellular {

class CellularService {
public:
    using ClockFn = std::function<uint64_t()>;

    // Der Transport wird geliehen und ueberlebt diesen Dienst.
    explicit CellularService(IAtTransport& at) : at_(at) {}

    void set_clock(ClockFn now) { now_ = std::move(now); }
    void set_config(const CellularConfig& c);
    const CellularConfig& config() const { return cfg_; }

    SimManager& sim() { return sim_; }

    // Eine Abfragerunde. Billig genug fuer einen Sekundentakt, weil sie
    // abbricht, sobald etwas fehlt: ohne Antwort kein SIM-Check, ohne SIM
    // keine Registrierung, ohne Registrierung keine Funkwerte.
    const CellularStatus& poll();

    const CellularStatus& status() const { return st_; }

private:
    uint64_t now() const { return now_ ? now_() : 0; }

    IAtTransport&  at_;
    CellularConfig cfg_;
    SimManager     sim_;
    CellularStatus st_;
    ClockFn        now_;
    bool           identity_read_ = false;
};

// Ein AT-Kommando so, wie es in ein Log darf.
//
// AT+CPIN="1234" wird zu AT+CPIN=<redigiert>. Dasselbe fuer die Kommandos, die
// ein APN-Passwort tragen. Die Regel ist bewusst breit: lieber ein Kommando zu
// viel unkenntlich als eines zu wenig.
std::string redact_at(const std::string& cmd);

}} // namespace machino::cellular
