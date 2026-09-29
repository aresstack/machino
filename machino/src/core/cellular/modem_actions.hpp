// Einmalige Modem-Aktionen auf Zuruf: Nachbarzellen, PIN-Sperre, Neustart.
//
// Portiert aus esp32-modem-host (modemNeighbourDump, modemSimPinManage,
// modemReset). Dort haengt jede davon an einem HTTP-Handler, der sofort AT
// schickt. Hier nicht: der HTTP-Thread hinterlegt eine Absicht, und der
// Hauptthread fuehrt sie im naechsten Takt aus -- ein Sprecher auf dem AT-Port,
// siehe cellular_uplink.hpp.
//
// Jede Absicht wird GENAU EINMAL ausgefuehrt und dann verbraucht, was immer
// dabei herauskommt. Das ist bei der PIN-Sperre keine Formalitaet: ein
// AT+CLCK mit falscher PIN kostet einen der drei Versuche der Karte, und eine
// Wiederholung "weil es beim ersten Mal nicht ging" waere der zweite.
//
// Was NICHT hier ist: eine freie AT-Konsole. Ein falsches Kommando stellt die
// USB-Komposition des Modems dauerhaft um, und die Seite sagt das auch.
#pragma once
#include "core/cellular/at_parse.hpp"
#include "core/cellular/cellular_status.hpp"
#include "ports/iat_transport.hpp"
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace machino { namespace cellular {

struct NeighbourReport {
    bool        have = false;
    uint64_t    updated_ms = 0;
    std::vector<NeighbourCell> cells;
    std::string raw;        // die Antwort, wie sie kam -- fuer den Fall, dass
                            // die Firmware andere Felder liefert als erwartet
    std::string error;
};

enum class SimLockAction { Status, Enable, Disable, Change };
const char* sim_lock_action_name(SimLockAction a);
bool        sim_lock_action_parse(const std::string& s, SimLockAction& out);

struct SimLockRequest {
    SimLockAction action = SimLockAction::Status;
    std::string   pin;        // GEHEIM: nur bis zur Ausfuehrung im Speicher
    std::string   new_pin;    // dito, nur bei Change
};

struct SimLockReport {
    bool          have = false;
    uint64_t      updated_ms = 0;
    SimLockAction action = SimLockAction::Status;
    bool          ok = false;
    std::string   detail;     // ohne PIN, ohne Rohtext
    MaybeInt      enabled;    // +CLCK: 1 = Karte verlangt eine PIN
    MaybeInt      pin_left, puk_left;
    std::string   cpin;       // "READY", "SIM PIN", ...
};

class ModemActions {
public:
    using ClockFn = std::function<uint64_t()>;

    void set_clock(ClockFn now) { now_ = std::move(now); }

    void request_neighbours() { nb_requested_ = true; }
    void request_restart()    { restart_requested_ = true; }

    // Prueft nur die Form (4-8 Ziffern) und dass nichts anderes wartet. Ob die
    // Karte die PIN nimmt, weiss erst die Ausfuehrung.
    bool request_sim_lock(const SimLockRequest& r, std::string& why_not);

    struct Outcome {
        bool        pin_changed = false;   // Enable/Change/Disable erfolgreich:
        std::string new_pin;               // die zu speichernde PIN ("" = loeschen)
        bool        restarted = false;     // AT+CFUN=1,1 ging hinaus
    };

    // Hoechstens EINE Aktion je Takt. Ohne antwortendes Modem werden wartende
    // Absichten als Fehler beantwortet und verbraucht, nicht aufgehoben: eine
    // PIN-Aktion, die Minuten spaeter von selbst losgeht, will niemand.
    Outcome tick(IAtTransport& at, const CellularStatus& status);

    const NeighbourReport& neighbours() const { return nb_; }
    const SimLockReport&   sim_lock() const   { return sl_; }
    bool restart_pending() const { return restart_requested_; }

private:
    uint64_t now() const { return now_ ? now_() : 0; }
    void run_sim_lock(IAtTransport& at, Outcome& out);
    void run_neighbours(IAtTransport& at);

    ClockFn         now_;
    NeighbourReport nb_;
    SimLockReport   sl_;
    bool            nb_requested_ = false;
    bool            restart_requested_ = false;
    bool            sim_requested_ = false;
    SimLockRequest  sim_req_;
};

}} // namespace machino::cellular
