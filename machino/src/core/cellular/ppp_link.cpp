#include "core/cellular/ppp_link.hpp"

namespace machino { namespace cellular {

const char* ppp_exit_name(PppExit e)
{
    switch (e) {
        case PppExit::None:              return "none";
        case PppExit::Ok:                return "ok";
        case PppExit::NoDevice:          return "no-device";
        case PppExit::DialFailed:        return "dial-failed";
        case PppExit::AuthFailed:        return "auth-failed";
        case PppExit::NegotiationFailed: return "negotiation-failed";
        case PppExit::LinkTerminated:    return "link-terminated";
        case PppExit::Other:             return "other";
    }
    return "other";
}

namespace {

// LCP/IPCP sind normalerweise in unter fuenf Sekunden durch. Die Referenz gibt
// nach zwoelf auf, mit der Begruendung, dass ein stiller Haenger von laengerem
// Warten nicht besser wird -- es kommt dann gar kein Ereignis mehr.
const uint32_t kNegotiateWaitMs = 12000;

// Nach dem Start von pppd, bevor ueberhaupt etwas vom Anruf zu sehen sein kann.
// Kuerzer als die Aushandlung, weil hier nur "laeuft der Prozess" gefragt ist.
const uint32_t kDialWaitMs = 8000;

} // namespace

void PppLink::enter(DataLinkState s, const std::string& detail)
{
    st_.state = s;
    st_.detail = detail;
}

void PppLink::fail(const std::string& detail)
{
    ++st_.attempts;
    next_due_ms_ = now() + backoff_ms(st_.attempts);
    // Aufraeumen gehoert zum Scheitern. Ein pppd, der nach einem Fehlschlag
    // weiterlaeuft, haelt den Modem-Port belegt -- und der naechste Versuch
    // findet dann einen Port, der nicht auf AT antwortet, und diagnostiziert
    // eine alte Datensession, die er selbst hinterlassen hat.
    if (started_) { be_.stop(); started_ = false; }
    enter(DataLinkState::Failed, detail);
}

bool PppLink::due() const
{
    return next_due_ms_ == 0 || now() >= next_due_ms_;
}

void PppLink::connect()
{
    if (want_up_) return;              // wiederholter Aufruf ist ein No-op
    want_up_ = true;
    st_.attempts = 0;
    next_due_ms_ = 0;
    enter(DataLinkState::WaitDevice, "Verbindung angefordert");
}

void PppLink::disconnect()
{
    want_up_ = false;
    if (started_) { be_.stop(); started_ = false; }
    st_.interface_name.clear();
    st_.address = LinkAddress{};
    st_.modem_pdp_address.clear();
    st_.attempts = 0;
    next_due_ms_ = 0;
    enter(DataLinkState::Disabled, "getrennt");
}

const CellularLinkState& PppLink::tick(const CellularStatus& status)
{
    if (!want_up_) {
        if (st_.state != DataLinkState::Disabled) disconnect();
        return st_;
    }

    // Was pppd gerade sagt. Zuerst, weil jede Entscheidung unten davon abhaengt
    // -- und weil ein Prozess, der zwischen zwei Ticks gestorben ist, sonst
    // eine ganze Runde lang als "laeuft" gelten wuerde.
    PppStatus ps;
    const bool know_ps = be_.status(ps);

    // Das Modem ist weg. Alles, was daran hing, ist damit auch weg.
    if (!status.present || !status.responsive) {
        if (started_) { be_.stop(); started_ = false; }
        st_.interface_name.clear();
        st_.address = LinkAddress{};
        enter(status.present ? DataLinkState::WaitAt : DataLinkState::WaitDevice,
              status.present ? "Modem antwortet nicht" : "kein Modem");
        return st_;
    }

    if (st_.state == DataLinkState::Failed && !due()) return st_;

    if (status.sim != SimState::Ready) {
        // NICHT waehlen. Der Dial liefe ins Leere, und die Meldung waere
        // "kein CONNECT" statt "PIN fehlt".
        enter(DataLinkState::WaitSim, status.sim_detail.empty()
              ? std::string("SIM nicht bereit") : status.sim_detail);
        return st_;
    }
    if (!reg_is_registered(status.registration)) {
        enter(DataLinkState::WaitRegistration,
              std::string("nicht im Netz: ") + reg_state_name(status.registration));
        return st_;
    }

    // ---- laeuft schon etwas? ---------------------------------------------
    if (started_) {
        if (!know_ps) {
            // Nicht nachsehen koennen ist nicht dasselbe wie "nichts laeuft".
            // Hier abzubauen hiesse, einen funktionierenden Anruf wegen eines
            // unlesbaren Zustands zu beenden.
            enter(st_.state, "Zustand des PPP-Prozesses nicht lesbar");
            return st_;
        }

        if (!ps.running) {
            // pppd ist weg. WARUM steht in seinem Exit-Code, und genau das ist
            // der Unterschied zwischen "SIM-Karte ohne Guthaben" und "falsches
            // Passwort" -- beides sieht sonst wie "keine Verbindung" aus.
            started_ = false;
            switch (ps.exit) {
                case PppExit::AuthFailed:
                    fail("Authentifizierung abgelehnt - Benutzername oder Passwort pruefen");
                    break;
                case PppExit::DialFailed:
                    fail("kein CONNECT auf " + modem_tty_ + " - Einwahlnummer oder APN pruefen");
                    break;
                case PppExit::NegotiationFailed:
                    fail("PPP-Aushandlung gescheitert");
                    break;
                case PppExit::NoDevice:
                    fail("der Modem-Port ist verschwunden");
                    break;
                case PppExit::LinkTerminated:
                    fail("die Gegenstelle hat die Verbindung beendet");
                    break;
                default:
                    fail(ps.detail.empty() ? std::string("pppd hat aufgehoert") : ps.detail);
                    break;
            }
            return st_;
        }

        // Laeuft. Gibt es schon ein Interface mit Adresse?
        if (!ps.ifname.empty() && ps.address.has_address()) {
            st_.interface_name = ps.ifname;
            st_.address = ps.address;
            st_.attempts = 0;
            next_due_ms_ = 0;
            enter(DataLinkState::Up, "verbunden ueber " + ps.ifname);
            return st_;
        }

        // Noch nicht. Aushandlung laeuft -- aber nicht endlos.
        st_.interface_name = ps.ifname;
        if (now() > negotiate_deadline_ms_) {
            // WIR brechen ab, nicht das Netz. Das steht so im Text, weil die
            // Referenz genau hier einen Tag Fehlersuche gekostet hat: es sah
            // nach einem Netzproblem aus und war eine haengende Aushandlung,
            // die nur ein Neuaufbau loest.
            fail("Aushandlung haengt (" + std::to_string(kNegotiateWaitMs / 1000) +
                 " s ohne Ergebnis) - Abbruch und neuer Versuch");
            return st_;
        }
        enter(ps.ifname.empty() ? DataLinkState::Dial : DataLinkState::Negotiating,
              ps.ifname.empty() ? "Einwahl laeuft" : "Aushandlung auf " + ps.ifname);
        return st_;
    }

    // ---- nichts laeuft: aufbauen -----------------------------------------
    if (modem_tty_.empty()) {
        enter(DataLinkState::WaitDevice, "kein Modem-Port gefunden");
        return st_;
    }
    if (cfg_.apn.empty()) {
        fail("kein APN konfiguriert - ohne den weiss das Netz nicht, wohin");
        return st_;
    }

    enter(DataLinkState::ConfigurePdp, "PDP-Kontext wird gesetzt");

    // AT+CGACT=0,1 auf dem AT-PORT, und das ist der Trick aus der Referenz.
    //
    // Der haeufige Stolperstein ist ein Modem, das nach einem Neustart des
    // Hosts noch in der alten PPP-Sitzung steckt: der Modem-Port nimmt dann
    // alles als Nutzdaten und antwortet auf nichts. Der AT-Port ist davon
    // unberuehrt, und ein Deaktivieren des Kontexts von dort beendet die alte
    // Sitzung -- der Modem-Port faellt in den Kommandomodus zurueck. In der
    // Referenz steht dazu: "vorher half nur Neu-Anstecken".
    //
    // OK oder ERROR sind hier beide in Ordnung: gibt es keinen aktiven
    // Kontext, ist nichts abzuraeumen.
    //
    // Die Escape-Sequenz (+++ / ATH) auf dem MODEM-Port macht machino
    // ausdruecklich NICHT selbst. Sie braucht eine Sekunde Ruhe davor und
    // danach, sonst gehen die drei Zeichen als Nutzdaten durch -- und sie
    // gehoert auf einen Port, den gleich pppd uebernimmt. Beides kann chat(8),
    // und es steht im Chat-Skript, das das Backend schreibt. Ein zweiter
    // Sprecher auf demselben seriellen Port waere der sicherste Weg, eine
    // funktionierende Einwahl zu zerlegen.
    at_.command("AT+CGACT=0,1", 10000);

    const std::string pdp = (cfg_.pdp == PdpType::Ipv4v6) ? "IPV4V6" : "IP";
    if (!at_.command("AT+CGDCONT=1,\"" + pdp + "\",\"" + cfg_.apn + "\"").ok()) {
        fail("das Modem hat den APN abgelehnt");
        return st_;
    }

    PppRequest req;
    req.tty      = modem_tty_;
    req.apn      = cfg_.apn;
    req.dial     = cfg_.dial.empty() ? std::string("*99***1#") : cfg_.dial;
    req.pdp      = cfg_.pdp;
    req.auth     = cfg_.auth;
    req.username = cfg_.username;
    req.password = cfg_.password;      // geht in eine 0600-Datei, nie in argv

    if (!be_.start(req).is_ok()) {
        fail("der PPP-Prozess liess sich nicht starten");
        return st_;
    }
    started_ = true;
    negotiate_deadline_ms_ = now() + kDialWaitMs + kNegotiateWaitMs;
    enter(DataLinkState::Dial, "Einwahl mit " + req.dial);
    return st_;
}

}} // namespace machino::cellular
