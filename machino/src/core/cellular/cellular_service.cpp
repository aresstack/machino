#include "core/cellular/cellular_service.hpp"

namespace machino { namespace cellular {

namespace {

bool starts_with_ci(const std::string& s, const char* p)
{
    const std::string pat(p);
    if (s.size() < pat.size()) return false;
    for (size_t i = 0; i < pat.size(); ++i) {
        char a = s[i], b = pat[i];
        if (a >= 'a' && a <= 'z') a = (char)(a - 'a' + 'A');
        if (b >= 'a' && b <= 'z') b = (char)(b - 'a' + 'A');
        if (a != b) return false;
    }
    return true;
}

} // namespace

std::string redact_at(const std::string& cmd)
{
    // Alles bis zum ersten '=' bleibt stehen, der Rest verschwindet. Damit
    // bleibt im Log sichtbar, WELCHES Kommando lief -- das ist der ganze
    // Zweck der Zeile -- ohne den Wert zu zeigen.
    static const char* kSecret[] = {
        "AT+CPIN=",          // SIM-PIN
        "AT+CLCK=",          // PIN-Sperre an/aus: traegt die PIN
        "AT+CPWD=",          // PIN aendern: traegt alte UND neue
        "AT+QICSGP=",        // APN samt Benutzer und Passwort
        "AT+CGAUTH=",        // Auth-Zugangsdaten
    };
    for (const char* p : kSecret) {
        if (starts_with_ci(cmd, p)) return std::string(p) + "<redigiert>";
    }
    return cmd;
}

void CellularService::set_config(const CellularConfig& c)
{
    // Aendert sich die PIN, darf wieder genau einmal versucht werden -- sonst
    // muesste jemand nach einem Tippfehler neu starten.
    if (c.sim_pin != cfg_.sim_pin) sim_.forget_attempt();
    cfg_ = c;
}

const CellularStatus& CellularService::poll()
{
    CellularStatus s;
    s.last_update_ms = now();
    s.present = at_.available();

    if (!s.present) {
        s.last_error = "kein AT-Port - Modem nicht erkannt";
        st_ = s;
        return st_;
    }

    // 1. Lebt es? Ein blankes AT ist die billigste Frage, die es gibt, und
    //    unterscheidet "Port da" von "Modem antwortet".
    const AtExchange ping = at_.command("AT", 2000);
    s.responsive = ping.ok();
    if (!s.responsive) {
        s.last_error = ping.device_gone ? "Modem verschwunden"
                     : ping.timed_out   ? "keine Antwort auf AT"
                                        : "AT abgelehnt";
        st_ = s;
        return st_;
    }

    // 2. Wer ist es? Identitaet und IMEI aendern sich nicht; einmal reicht.
    //    Das spart bei jedem Durchlauf drei Kommandos.
    if (identity_read_) {
        s.identity = st_.identity;
        s.imei     = st_.imei;
    } else {
        s.identity = parse_ati(at_.command("ATI").payload);
        s.imei     = first_numeric_line(at_.command("AT+CGSN").payload);
        if (!s.identity.model.empty() || !s.imei.empty()) identity_read_ = true;
    }

    // 3. Ist die SIM bereit? Hier und nur hier darf eine PIN gesendet werden.
    const SimSnapshot sim = sim_.ensure_ready(at_, cfg_.sim_pin);
    s.sim        = sim.state;
    s.sim_detail = sim.detail;

    if (s.sim != SimState::Ready) {
        // ICCID liest sich auch ohne entsperrte SIM; IMSI nicht, das ist ein
        // Teilnehmermerkmal und braucht die entsperrte Karte. Es hier trotzdem
        // abzufragen ergaebe nur ein CME ERROR pro Durchlauf.
        s.iccid = at_extract(at_.command("AT+QCCID").raw, "+QCCID:");
        s.last_error = sim.detail;
        st_ = s;
        return st_;
    }
    s.iccid = at_extract(at_.command("AT+QCCID").raw, "+QCCID:");
    s.imsi  = first_numeric_line(at_.command("AT+CIMI").payload);

    // 4. Im Netz?
    s.registration = parse_cereg(at_.command("AT+CEREG?").raw);

    // 5. Funkwerte. CSQ geht immer, der Rest ist ohne Registrierung leer oder
    //    irrefuehrend -- ein Betreibername ohne Registrierung ist der zuletzt
    //    gesehene, nicht der aktuelle.
    s.signal = parse_csq(at_.command("AT+CSQ").raw);

    if (reg_is_registered(s.registration)) {
        const OperatorInfo op = parse_cops(at_.command("AT+COPS?").raw);
        s.operator_name = op.name;

        const NwInfo nw = parse_qnwinfo(at_.command("AT+QNWINFO").raw);
        s.rat = nw.rat;
        s.operator_code = nw.oper_code;

        s.cell = parse_qeng_servingcell(at_.command("AT+QENG=\"servingcell\"").raw);
        s.pdp  = parse_cgpaddr(at_.command("AT+CGPADDR").raw);
    } else {
        s.last_error = std::string("nicht registriert: ") + reg_state_name(s.registration);
    }

    st_ = s;
    return st_;
}

}} // namespace machino::cellular
