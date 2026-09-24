#include "core/cellular/sim_manager.hpp"

#include <cstdint>
#include <cstdio>

namespace machino { namespace cellular {

namespace {

// FNV-1a. Es geht nicht um kryptografische Staerke, sondern darum, dass in der
// Notizdatei kein Klartext steht: gespeichert wird, DASS mit einer PIN dieses
// Inhalts bereits versucht wurde, nicht welche es war. Bei vierstelligen
// PINs ist der Raum klein genug, dass jede Hashfunktion durchprobierbar bleibt
// -- deshalb liegt die Notiz auch nicht dort, wo Konfiguration liegt, sondern
// im fluechtigen Laufzeitverzeichnis.
uint32_t fnv1a(const std::string& s)
{
    uint32_t h = 2166136261u;
    for (unsigned char c : s) { h ^= c; h *= 16777619u; }
    return h;
}

} // namespace

std::string sim_attempt_token(const std::string& pin)
{
    if (pin.empty()) return std::string();
    char buf[32];
    std::snprintf(buf, sizeof buf, "%u:%08x", (unsigned)pin.size(), fnv1a(pin));
    return buf;
}

void SimManager::set_persistence(LoadFn load, StoreFn store)
{
    load_ = std::move(load);
    store_ = std::move(store);
    loaded_ = false;
}

bool SimManager::already_failed_for(const std::string& pin) const
{
    const std::string tok = sim_attempt_token(pin);
    return !tok.empty() && tok == attempt_log_;
}

void SimManager::remember_attempt(const std::string& pin)
{
    attempt_log_ = sim_attempt_token(pin);
    if (store_) store_(attempt_log_);
}

void SimManager::forget_attempt()
{
    attempt_log_.clear();
    if (store_) store_(std::string());
}

SimSnapshot SimManager::ensure_ready(IAtTransport& at, const std::string& pin)
{
    if (!loaded_ && load_) {
        std::string tok;
        if (load_(tok)) attempt_log_ = tok;
        loaded_ = true;
    }

    SimSnapshot s;
    const AtExchange r = at.command("AT+CPIN?");
    s.state = parse_cpin(r.raw);

    if (r.device_gone) {
        s.state = SimState::Unknown;
        s.detail = "Modem nicht erreichbar";
        last_ = s;
        return s;
    }
    if (r.timed_out) {
        s.state = SimState::Unknown;
        s.detail = "keine Antwort auf AT+CPIN?";
        last_ = s;
        return s;
    }

    switch (s.state) {
        case SimState::Ready:
            // Auch hier loeschen, nicht nur direkt nach einem Entsperren:
            // Entsperren und Re-Attach brauchen Sekunden, und dann meldet
            // erst eine spaetere Runde READY. Ohne das bliebe die Notiz
            // stehen und eine funktionierende PIN waere fuer den Rest des
            // Lebenszyklus gesperrt.
            forget_attempt();
            s.detail = "SIM bereit";
            last_ = s;
            return s;
        case SimState::PukRequired:
            // NIEMALS ein automatischer Versuch. Die PIN-Versuche sind bereits
            // aufgebraucht; ein weiterer CPIN-Befehl hilft nicht und der PUK
            // gehoert nicht in eine Konfigurationsdatei.
            s.detail = "SIM gesperrt - PUK noetig, nur ausserhalb der Kamera entsperrbar";
            last_ = s;
            return s;
        case SimState::NotInserted:
            s.detail = "keine SIM erkannt";
            last_ = s;
            return s;
        case SimState::PinRequired:
            break;                       // weiter unten
        case SimState::Unknown:
        case SimState::Error:
            s.detail = "SIM-Zustand unklar";
            last_ = s;
            return s;
    }

    // Ab hier: die SIM verlangt eine PIN.
    if (pin.empty()) {
        s.detail = "SIM verlangt eine PIN, es ist keine konfiguriert";
        last_ = s;
        return s;
    }
    if (already_failed_for(pin)) {
        // Der Kern des Ganzen. Ein Supervisor, der im Sekundentakt anklopft,
        // darf diese Zeile beliebig oft erreichen -- gesendet wird nichts.
        s.pin_attempted = true;
        s.pin_rejected = true;
        s.detail = "PIN wurde bereits abgelehnt - kein weiterer Versuch (PUK-Schutz). "
                   "PIN pruefen, dann neu starten";
        last_ = s;
        return s;
    }

    // Die Notiz wird VOR dem Senden gesetzt, nicht danach.
    //
    // Sonst haengt der ganze Schutz daran, dass das Modem eine falsche PIN mit
    // ERROR beantwortet. Tut es das nicht -- OK melden und trotzdem gesperrt
    // bleiben ist bei Mobilfunkmodems keine Seltenheit --, dann wird nichts
    // vermerkt, die naechste Runde sieht wieder "SIM PIN" und schickt dieselbe
    // PIN erneut. Drei Runden, und die Karte will den PUK. Genau das soll hier
    // nicht passieren koennen, also zaehlt der VERSUCH, nicht sein Ausgang.
    //
    // Und der Versuch ueberlebt damit auch einen Absturz zwischen Senden und
    // Auswerten.
    s.pin_attempted = true;
    remember_attempt(pin);

    const AtExchange unlock = at.command("AT+CPIN=\"" + pin + "\"", 10000);
    if (!unlock.ok()) {
        s.pin_rejected = true;
        // Der Rohtext der Antwort wird NICHT uebernommen: er kann die
        // gesendete PIN enthalten, wenn das Modem das Kommando echot.
        s.detail = "PIN abgelehnt";
        last_ = s;
        return s;
    }

    // Entsperren und Re-Attach brauchen einen Moment. Das Original wartet bis
    // zu sechs Sekunden in Halbsekundenschritten; hier fragt der Aufrufer im
    // naechsten Durchlauf erneut, statt zu schlafen -- dieser Prozess bedient
    // noch andere Dinge.
    const AtExchange after = at.command("AT+CPIN?");
    s.state = parse_cpin(after.raw);
    if (s.state == SimState::Ready) {
        // Erfolg loescht die Notiz: eine funktionierende PIN darf beim
        // naechsten Mal wieder verwendet werden.
        forget_attempt();
        s.pin_rejected = false;
        s.detail = "SIM bereit (mit PIN entsperrt)";
    } else {
        s.detail = "PIN gesendet, SIM meldet noch kein READY";
    }
    last_ = s;
    return s;
}

}} // namespace machino::cellular
