// SIM-Bereitschaft, und der Schutz der Karte davor.
//
// Portiert aus esp32-modem-host/modem_sim.cpp. Die tragende Regel dort, im
// Original kommentiert und hier woertlich uebernommen:
//
//     die konfigurierte PIN wird je Boot GENAU EINMAL gesendet. Ein
//     abgelehnter Versuch wird nicht wiederholt (drei falsche Versuche
//     sperren die SIM -> PUK), auch nicht durch Supervisor-Retries.
//
// Ein Supervisor, der alle paar Sekunden "stell die Verbindung her" sagt, ist
// der Normalfall und nicht der Fehler. Der Fehler waere, ihn eine abgelehnte
// PIN dreimal schicken zu lassen -- danach braucht der Besitzer seinen PUK,
// und den hat niemand zur Hand.
//
// ZWEI Unterschiede zum ESP32, beide aus der Plattform:
//
//   1. Der Zaehler haengt dort an `static bool` und damit am Prozess. Auf dem
//      ESP32 ist das dasselbe wie "je Boot" -- es gibt einen Prozess und er
//      wird nicht neu gestartet. Auf Linux wird er das sehr wohl. Deshalb
//      kann dieser Zustand nach aussen gegeben und ueber einen Neustart
//      hinweg gehalten werden (siehe `attempt_log_`), und zwar an einem Ort,
//      der einen Reboot NICHT ueberlebt: nach einem echten Neustart darf
//      wieder genau einmal versucht werden.
//
//   2. Der Zaehler haengt zusaetzlich am PIN-WERT. Eine korrigierte PIN darf
//      wieder versuchen, dieselbe abgelehnte nicht. Das ist im Original schon
//      so (`s_pinTriedFor`) und wird hier beibehalten -- ohne das muesste der
//      Besitzer nach einem Tippfehler neu starten.
#pragma once
#include "core/cellular/at_parse.hpp"
#include "ports/iat_transport.hpp"
#include <functional>
#include <string>

namespace machino { namespace cellular {

struct SimSnapshot {
    SimState    state = SimState::Unknown;
    std::string detail;          // Klartext fuer die Anzeige, ohne Geheimnisse
    bool        pin_attempted = false;   // in diesem Lebenszyklus schon versucht
    bool        pin_rejected = false;    // und abgelehnt -> kein weiterer Versuch
};

class SimManager {
public:
    // `load`/`store` halten die Notiz "PIN schon versucht" ueber einen
    // Prozessneustart. Beide duerfen leer sein; dann gilt der Schutz nur
    // innerhalb dieses Prozesses, was besser ist als nichts, aber schlechter
    // als das hier.
    //
    // Gespeichert wird NICHT die PIN, sondern nur, dass ein Versuch mit einer
    // PIN dieses Inhalts fehlgeschlagen ist -- als Laenge und Pruefsumme, die
    // den Wert nicht zurueckgibt. Ein Angreifer mit Lesezugriff auf die Datei
    // lernt daraus nicht, wie die PIN lautet.
    using LoadFn  = std::function<bool(std::string& token)>;
    using StoreFn = std::function<void(const std::string& token)>;

    void set_persistence(LoadFn load, StoreFn store);

    // Prueft AT+CPIN? und entsperrt, falls noetig und erlaubt.
    //
    // `pin` leer = keine konfiguriert. Dann wird bei PIN_REQUIRED NICHTS
    // gesendet; der Zustand bleibt stehen und sagt, was fehlt.
    SimSnapshot ensure_ready(IAtTransport& at, const std::string& pin);

    const SimSnapshot& last() const { return last_; }

    // Nach einer Aenderung der PIN-Konfiguration durch den Benutzer: der
    // Schutz gilt pro PIN-Wert, also darf eine NEUE PIN wieder versuchen.
    void forget_attempt();

private:
    bool already_failed_for(const std::string& pin) const;
    void remember_attempt(const std::string& pin);   // merkt den VERSUCH, nicht seinen Ausgang

    SimSnapshot last_;
    std::string attempt_log_;    // "<len>:<hash>" der zuletzt versuchten PIN
    LoadFn      load_;
    StoreFn     store_;
    bool        loaded_ = false;
};

// Sichtbar fuer den Test: die Notiz darf die PIN nicht enthalten.
std::string sim_attempt_token(const std::string& pin);

}} // namespace machino::cellular
