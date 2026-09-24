#include "core/cellular/at_parse.hpp"

#include <cstdlib>

namespace machino { namespace cellular {

namespace {

std::string trim(const std::string& s)
{
    size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n')) ++b;
    while (e > b && (s[e-1] == ' ' || s[e-1] == '\t' || s[e-1] == '\r' || s[e-1] == '\n')) --e;
    return s.substr(b, e - b);
}

std::vector<std::string> lines_of(const std::string& raw)
{
    std::vector<std::string> out;
    size_t start = 0;
    for (size_t i = 0; i <= raw.size(); ++i) {
        if (i == raw.size() || raw[i] == '\n') {
            out.push_back(trim(raw.substr(start, i - start)));
            start = i + 1;
        }
    }
    return out;
}

// Eine Zahl, aber nur wenn das Feld auch wirklich eine ist. QENG liefert fuer
// unbekannte Werte "-" oder leer, und strtol() macht daraus eine 0.
MaybeInt as_int(const std::string& field)
{
    const std::string s = trim(field);
    if (s.empty()) return MaybeInt();
    size_t i = 0;
    if (s[0] == '-' || s[0] == '+') i = 1;
    if (i >= s.size()) return MaybeInt();           // nur ein Vorzeichen
    for (size_t k = i; k < s.size(); ++k)
        if (s[k] < '0' || s[k] > '9') return MaybeInt();
    return MaybeInt((int)::strtol(s.c_str(), nullptr, 10));
}

struct BandMhz { int band; int mhz; };
// Aus LTE_BANDS (esp32-modem-host/ec200a_modem.cpp): die Baender, die dieses
// Modul kann, nicht alle die es gibt.
const BandMhz kBands[] = {
    {  1, 2100 }, {  3, 1800 }, {  5,  850 }, {  7, 2600 }, {  8,  900 },
    { 20,  800 }, { 28,  700 }, { 38, 2600 }, { 40, 2300 }, { 41, 2500 },
};

// Die erste Zahl in einem Feld wie "3" oder "LTE BAND 3".
MaybeInt leading_number(const std::string& s)
{
    int n = 0; bool any = false;
    for (char c : s) {
        if (c >= '0' && c <= '9') { n = n * 10 + (c - '0'); any = true; }
        else if (any) break;
    }
    return any ? MaybeInt(n) : MaybeInt();
}

} // namespace

std::string at_extract(const std::string& raw, const std::string& prefix)
{
    const size_t p = raw.find(prefix);
    if (p == std::string::npos) return std::string();
    const size_t start = p + prefix.size();
    size_t end = raw.find('\n', start);
    if (end == std::string::npos) end = raw.size();
    return trim(raw.substr(start, end - start));
}

std::string csv_field(const std::string& line, int idx)
{
    int field = 0;
    bool quoted = false;
    std::string cur;
    for (char c : line) {
        if (c == '"') { quoted = !quoted; continue; }
        if (c == ',' && !quoted) {
            if (field == idx) return trim(cur);
            ++field; cur.clear(); continue;
        }
        cur += c;
    }
    if (field == idx) return trim(cur);
    return std::string();
}

std::string first_numeric_line(const std::string& raw)
{
    for (const std::string& l : lines_of(raw)) {
        if (l.size() < 6) continue;
        bool all_digit = true;
        for (char c : l) if (c < '0' || c > '9') { all_digit = false; break; }
        if (all_digit) return l;
    }
    return std::string();
}

int lte_band_mhz(int band)
{
    for (const BandMhz& b : kBands) if (b.band == band) return b.mhz;
    return 0;
}

// ------------------------------------------------------------------ ATI ---

ModemIdentity parse_ati(const std::string& raw)
{
    ModemIdentity id;
    id.firmware = at_extract(raw, "Revision:");

    // Das Referenzprojekt sucht "Quectel" und nimmt die Zeile DANACH als
    // Modell. Hier zusaetzlich: die gefundene Zeile ist der Hersteller, und
    // Zeilen, die schon Teil der Antwortstruktur sind, werden uebersprungen.
    const std::vector<std::string> ls = lines_of(raw);
    for (size_t i = 0; i < ls.size(); ++i) {
        if (ls[i].empty()) continue;
        if (ls[i].find("Quectel") == std::string::npos) continue;
        id.manufacturer = ls[i];
        for (size_t k = i + 1; k < ls.size(); ++k) {
            if (ls[k].empty()) continue;
            if (ls[k].compare(0, 9, "Revision:") == 0) break;
            id.model = ls[k];
            break;
        }
        break;
    }
    return id;
}

// ----------------------------------------------------------------- CPIN ---

const char* sim_state_name(SimState s)
{
    switch (s) {
        case SimState::Ready:        return "ready";
        case SimState::PinRequired:  return "pin_required";
        case SimState::PukRequired:  return "puk_required";
        case SimState::NotInserted:  return "not_inserted";
        case SimState::Error:        return "error";
        case SimState::Unknown:      break;
    }
    return "unknown";
}

SimState parse_cpin(const std::string& raw)
{
    // Reihenfolge zaehlt: "SIM PUK" enthaelt nicht "SIM PIN", aber
    // "SIM PIN2"/"SIM PUK2" sind eigene Zustaende, die NICHT den Datenzugang
    // sperren. Sie als PIN_REQUIRED zu lesen wuerde einen automatischen
    // CPIN-Versuch mit der falschen PIN ausloesen.
    if (raw.find("READY") != std::string::npos)    return SimState::Ready;
    if (raw.find("SIM PUK2") != std::string::npos) return SimState::Unknown;
    if (raw.find("SIM PIN2") != std::string::npos) return SimState::Unknown;
    if (raw.find("SIM PUK") != std::string::npos)  return SimState::PukRequired;
    if (raw.find("SIM PIN") != std::string::npos)  return SimState::PinRequired;
    if (raw.find("CME ERROR") != std::string::npos) return SimState::NotInserted;
    if (raw.empty()) return SimState::Unknown;
    return SimState::Error;
}

// ---------------------------------------------------------------- CEREG ---

const char* reg_state_name(RegState s)
{
    switch (s) {
        case RegState::NotRegistered:     return "not_registered";
        case RegState::RegisteredHome:    return "registered_home";
        case RegState::Searching:         return "searching";
        case RegState::Denied:            return "registration_denied";
        case RegState::RegisteredRoaming: return "registered_roaming";
        case RegState::Unknown:           break;
    }
    return "unknown";
}

bool reg_is_registered(RegState s)
{
    return s == RegState::RegisteredHome || s == RegState::RegisteredRoaming;
}

RegState parse_cereg(const std::string& raw)
{
    const std::string body = at_extract(raw, "+CEREG:");
    if (body.empty()) return RegState::Unknown;
    const MaybeInt stat = as_int(csv_field(body, 1));
    if (!stat.has) return RegState::Unknown;
    switch (stat.value) {
        case 0: return RegState::NotRegistered;
        case 1: return RegState::RegisteredHome;
        case 2: return RegState::Searching;
        case 3: return RegState::Denied;
        case 5: return RegState::RegisteredRoaming;
        default: return RegState::Unknown;    // 4 = unknown, alles andere auch
    }
}

// ------------------------------------------------------------------ CSQ ---

SignalInfo parse_csq(const std::string& raw)
{
    SignalInfo s;
    const std::string body = at_extract(raw, "+CSQ:");
    if (body.empty()) return s;
    const MaybeInt v = as_int(csv_field(body, 0));
    // 99 heisst im Standard "nicht bekannt oder nicht erfassbar". Als Zahl
    // weitergereicht waere es ein sehr gutes Signal.
    if (!v.has || v.value == 99 || v.value < 0 || v.value > 31) return s;
    s.csq = v;
    s.rssi_dbm = MaybeInt(-113 + 2 * v.value);
    return s;
}

// ----------------------------------------------------------------- COPS ---

OperatorInfo parse_cops(const std::string& raw)
{
    OperatorInfo o;
    const std::string body = at_extract(raw, "+COPS:");
    if (body.empty()) return o;
    o.name = csv_field(body, 2);
    o.act  = as_int(csv_field(body, 3));
    return o;
}

// -------------------------------------------------------------- QNWINFO ---

NwInfo parse_qnwinfo(const std::string& raw)
{
    NwInfo n;
    const std::string body = at_extract(raw, "+QNWINFO:");
    if (body.empty()) return n;
    n.rat       = csv_field(body, 0);
    n.oper_code = csv_field(body, 1);
    n.band      = csv_field(body, 2);
    n.channel   = as_int(csv_field(body, 3));
    return n;
}

// ----------------------------------------------------------------- QENG ---

ServingCell parse_qeng_servingcell(const std::string& raw)
{
    ServingCell c;
    const std::string body = at_extract(raw, "+QENG:");
    if (body.empty()) return c;
    // Feld 0 ist "servingcell". Steht dort etwas anderes -- QENG kann auch
    // "neighbourcell" liefern --, gehoert die Zeile nicht hierher.
    if (csv_field(body, 0) != "servingcell") return c;

    c.state   = csv_field(body, 1);
    c.rat     = csv_field(body, 2);
    c.mcc     = csv_field(body, 4);
    c.mnc     = csv_field(body, 5);
    c.cell_id = csv_field(body, 6);
    c.pci     = as_int(csv_field(body, 7));
    c.earfcn  = as_int(csv_field(body, 8));
    c.band    = leading_number(csv_field(body, 9));
    c.tac     = csv_field(body, 12);
    c.rsrp    = as_int(csv_field(body, 13));
    c.rsrq    = as_int(csv_field(body, 14));
    c.rssi    = as_int(csv_field(body, 15));
    c.sinr    = as_int(csv_field(body, 16));

    if (c.band.has) {
        const int mhz = lte_band_mhz(c.band.value);
        if (mhz) c.band_mhz = MaybeInt(mhz);
    }
    return c;
}

// -------------------------------------------------------------- CGPADDR ---

PdpAddress parse_cgpaddr(const std::string& raw)
{
    PdpAddress a;
    const std::string body = at_extract(raw, "+CGPADDR:");
    if (body.empty()) return a;
    const std::string f1 = csv_field(body, 1);
    const std::string f2 = csv_field(body, 2);
    // Ein nicht aktivierter Kontext liefert "0.0.0.0" -- das ist keine
    // Adresse, sondern die Abwesenheit einer.
    if (!f1.empty() && f1 != "0.0.0.0") a.ipv4 = f1;
    if (!f2.empty() && f2 != "0.0.0.0") a.ipv6 = f2;
    return a;
}

// ------------------------------------------------------------- CGCONTRDP ---

namespace {

// "10.1.2.3.255.255.255.0" -> Adresse und Maske trennen.
//
// 3GPP 27.007 haengt bei IPv4 die Subnetzmaske als vier weitere Oktette an
// dieselbe Punktliste. Acht Zahlen heissen also Adresse+Maske, vier nur
// Adresse. Alles andere (IPv6 kommt als 16 oder 32 Oktette) lassen wir hier
// stehen, statt daraus etwas Falsches zu machen.
void split_addr_mask(const std::string& field, std::string& addr, std::string& mask)
{
    addr.clear(); mask.clear();
    std::vector<std::string> parts;
    std::string cur;
    for (char c : field) {
        if (c == '.') { parts.push_back(cur); cur.clear(); }
        else cur += c;
    }
    if (!cur.empty()) parts.push_back(cur);

    auto join = [&](size_t from, size_t to) {
        std::string s;
        for (size_t i = from; i < to; ++i) { if (!s.empty()) s += '.'; s += parts[i]; }
        return s;
    };
    if (parts.size() == 8) { addr = join(0, 4); mask = join(4, 8); }
    else if (parts.size() == 4) { addr = join(0, 4); }
}

} // namespace

PdpContextParams parse_cgcontrdp(const std::string& raw)
{
    PdpContextParams p;
    const std::string body = at_extract(raw, "+CGCONTRDP:");
    if (body.empty()) return p;

    // +CGCONTRDP: <cid>,<bearer>,<apn>,<addr+mask>,<gw>,<dns1>,<dns2>
    //              0     1        2     3           4    5      6
    //
    // Das Referenzprojekt zaehlt hier ANFUEHRUNGSZEICHEN-Felder (atQuoted,
    // 1-basiert), nicht Kommaspalten: dort ist die Adresse Nummer 2, weil cid
    // und bearer unquotiert sind. Beim Uebertragen auf csv_field verschiebt
    // sich das um zwei -- einmal falsch gezaehlt, und die Kamera liest den APN
    // als ihre IP-Adresse.
    p.apn = csv_field(body, 2);
    std::string addr, mask;
    split_addr_mask(csv_field(body, 3), addr, mask);
    p.ipv4 = (addr == "0.0.0.0") ? std::string() : addr;
    p.netmask = mask;
    p.gateway = csv_field(body, 4);
    p.dns1 = csv_field(body, 5);
    p.dns2 = csv_field(body, 6);
    if (p.gateway == "0.0.0.0") p.gateway.clear();
    return p;
}

// ------------------------------------------------------------------ QCFG ---

MaybeInt parse_qcfg_int(const std::string& raw, const std::string& name)
{
    const std::string body = at_extract(raw, "+QCFG:");
    if (body.empty()) return MaybeInt();
    // Feld 0 ist der Name, in Anfuehrungszeichen. csv_field entfernt sie.
    if (csv_field(body, 0) != name) return MaybeInt();
    return as_int(csv_field(body, 1));
}

}} // namespace machino::cellular
