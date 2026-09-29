#include "core/cellular/modem_actions.hpp"

namespace machino { namespace cellular {

namespace {

bool pin_well_formed(const std::string& pin)
{
    if (pin.size() < 4 || pin.size() > 8) return false;
    for (char c : pin) if (c < '0' || c > '9') return false;
    return true;
}

// Nur der Fehlercode, nie der Rohtext: das Echo eines AT+CLCK traegt die PIN.
std::string cme_of(const AtExchange& x)
{
    const std::string cme = at_extract(x.raw, "+CME ERROR:");
    if (!cme.empty()) return " (+CME ERROR: " + cme + ")";
    if (x.timed_out)  return " (no answer)";
    if (x.device_gone) return " (modem gone)";
    return "";
}

} // namespace

const char* sim_lock_action_name(SimLockAction a)
{
    switch (a) {
        case SimLockAction::Enable:  return "enable";
        case SimLockAction::Disable: return "disable";
        case SimLockAction::Change:  return "change";
        case SimLockAction::Status:  break;
    }
    return "status";
}

bool sim_lock_action_parse(const std::string& s, SimLockAction& out)
{
    if (s == "status")  { out = SimLockAction::Status;  return true; }
    if (s == "enable")  { out = SimLockAction::Enable;  return true; }
    if (s == "disable") { out = SimLockAction::Disable; return true; }
    if (s == "change")  { out = SimLockAction::Change;  return true; }
    return false;
}

bool ModemActions::request_sim_lock(const SimLockRequest& r, std::string& why_not)
{
    if (sim_requested_) { why_not = "a SIM action is already waiting"; return false; }
    switch (r.action) {
        case SimLockAction::Enable:
        case SimLockAction::Disable:
            if (!pin_well_formed(r.pin)) { why_not = "a SIM PIN is 4 to 8 digits"; return false; }
            break;
        case SimLockAction::Change:
            if (!pin_well_formed(r.pin) || !pin_well_formed(r.new_pin)) {
                why_not = "the current and the new PIN are 4 to 8 digits each"; return false;
            }
            break;
        case SimLockAction::Status:
            break;
    }
    sim_req_ = r;
    sim_requested_ = true;
    return true;
}

ModemActions::Outcome ModemActions::tick(IAtTransport& at, const CellularStatus& s)
{
    Outcome out;
    if (!s.present || !s.responsive) {
        // Verbrauchen, nicht aufheben (siehe Kopfkommentar).
        if (sim_requested_) {
            sim_requested_ = false;
            sl_ = SimLockReport{};
            sl_.have = true; sl_.updated_ms = now();
            sl_.action = sim_req_.action;
            sl_.detail = "no modem answering - nothing sent";
            sim_req_ = SimLockRequest{};
        }
        if (nb_requested_) {
            nb_requested_ = false;
            nb_ = NeighbourReport{};
            nb_.have = true; nb_.updated_ms = now();
            nb_.error = "no modem answering";
        }
        restart_requested_ = false;
        return out;
    }

    if (restart_requested_) {
        restart_requested_ = false;
        // Der Neustart der Referenz: AT+CFUN=1,1. Das Modem enumeriert neu; der
        // Datenlink sieht es verschwinden und faengt von vorn an -- derselbe
        // Weg wie nach einem Moduswechsel, nichts Neues.
        at.command("AT+CFUN=1,1", 10000);
        out.restarted = true;
        return out;
    }
    if (sim_requested_) {
        sim_requested_ = false;
        run_sim_lock(at, out);
        // Die PIN hat ihren Dienst getan.
        sim_req_ = SimLockRequest{};
        return out;
    }
    if (nb_requested_) {
        nb_requested_ = false;
        run_neighbours(at);
    }
    return out;
}

void ModemActions::run_sim_lock(IAtTransport& at, Outcome& out)
{
    SimLockReport r;
    r.have = true;
    r.updated_ms = now();
    r.action = sim_req_.action;

    switch (sim_req_.action) {
        case SimLockAction::Enable:
        case SimLockAction::Disable: {
            const bool enable = (sim_req_.action == SimLockAction::Enable);
            const AtExchange x = at.command(
                std::string("AT+CLCK=\"SC\",") + (enable ? "1" : "0") + ",\"" + sim_req_.pin + "\"", 10000);
            r.ok = x.ok();
            if (r.ok) {
                // Wie die Referenz: die PIN wird fuer den Verbindungsaufbau
                // mitgefuehrt, damit der naechste Start ohne weitere
                // Nutzeraktion klappt; beim Deaktivieren wird sie geloescht.
                out.pin_changed = true;
                out.new_pin = enable ? sim_req_.pin : std::string();
                r.detail = enable ? "PIN lock enabled - the PIN is stored for the next connection"
                                  : "PIN lock disabled - the stored PIN was cleared";
            } else {
                r.detail = std::string("the card rejected the request") + cme_of(x) +
                           " - a wrong PIN costs one of the three attempts";
            }
            break;
        }
        case SimLockAction::Change: {
            const AtExchange x = at.command(
                "AT+CPWD=\"SC\",\"" + sim_req_.pin + "\",\"" + sim_req_.new_pin + "\"", 10000);
            r.ok = x.ok();
            if (r.ok) {
                out.pin_changed = true;
                out.new_pin = sim_req_.new_pin;
                r.detail = "PIN changed - the new PIN is stored for the next connection";
            } else {
                r.detail = std::string("the card rejected the change") + cme_of(x) +
                           " - the lock must be enabled and the current PIN correct";
            }
            break;
        }
        case SimLockAction::Status:
            r.ok = true;
            r.detail = "status read";
            break;
    }

    // Den Stand immer mitliefern, was auch immer die Aktion war.
    r.enabled = parse_clck(at.command("AT+CLCK=\"SC\",2").raw);
    const PinCounters pc = parse_qpinc(at.command("AT+QPINC=\"SC\"").raw);
    r.pin_left = pc.pin_left;
    r.puk_left = pc.puk_left;
    r.cpin = at_extract(at.command("AT+CPIN?").raw, "+CPIN:");
    sl_ = r;
}

void ModemActions::run_neighbours(IAtTransport& at)
{
    NeighbourReport r;
    r.have = true;
    r.updated_ms = now();
    // Die Nachbarzellenliste braucht einen Moment; die Referenz nimmt dafuer
    // dieselbe Abfrage, roh angezeigt.
    const AtExchange x = at.command("AT+QENG=\"neighbourcell\"", 8000);
    r.raw = x.raw;
    if (!x.ok()) {
        r.error = x.timed_out ? "no answer from the modem"
                : x.device_gone ? "modem gone"
                                : "the modem rejected the query";
    } else {
        r.cells = parse_qeng_neighbours(x.raw);
        if (r.cells.empty()) r.error = "no neighbour cells reported";
    }
    nb_ = r;
}

}} // namespace machino::cellular
