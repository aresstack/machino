// machinoctl -- der plattformneutrale Teil: Befehle, Feldtabelle, Darstellung,
// Konsole. Kein Socket, kein Terminal hier (das ist machinoctl_linux.cpp), damit
// alles hier gegen einen Fake-HTTP-Client hosttestbar ist.
#include "app/ctl/machinoctl.hpp"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace machino { namespace ctl {

namespace {

// ---- Feldtabelle: exakt die Schnittmenge, die PUT /api/v1/ipsec/config kennt
// (ipsec_api.cpp `known[]`). Ein Feld, das hier fehlt, kann die Konsole nicht
// setzen -- absichtlich, damit ein Tippfehler nie als "gespeichert" durchgeht.
enum class Kind { Bool, Int, Str, Enum, Algos, Secret, Pem };
struct Field {
    const char* name;      // kanonischer API-Name
    Kind        kind;
    const char* allowed;   // Enum: a|b|c, Int: lo..hi, Algos: erlaubte Werte
    const char* help;
    long long   lo, hi;    // nur Int
};
const Field kFields[] = {
    {"enabled",        Kind::Bool,   "true|false",   "VPN aktiv (Voraussetzung fuer connect)", 0, 0},
    {"gateway",        Kind::Str,    "",             "Hostname oder IPv4 des IKE-Gateways", 0, 0},
    {"port",           Kind::Int,    "1..65535",     "IKE-Port (NAT-T wechselt selbst auf 4500)", 1, 65535},
    {"underlay",       Kind::Enum,   "auto|ethernet|wifi|cellular", "Uplink fuer IKE/ESP (auto = Machino-Policy)", 0, 0},
    {"localId",        Kind::Str,    "",             "eigene IKE-Identitaet (FQDN-artig)", 0, 0},
    {"remoteId",       Kind::Str,    "",             "IKE-Identitaet des Gateways", 0, 0},
    {"localSubnet",    Kind::Str,    "",             "innere Adresse/Netz, CIDR (z.B. 10.77.0.2/32)", 0, 0},
    {"remoteSubnet",   Kind::Str,    "",             "Netz hinter dem Gateway, CIDR (genau eines)", 0, 0},
    {"natT",           Kind::Bool,   "true|false",   "NAT-Traversal erlauben", 0, 0},
    {"dpdIntervalS",   Kind::Int,    "0..3600",      "Dead-Peer-Detection in s (0 = aus)", 0, 3600},
    {"ikeLifetimeS",   Kind::Int,    "0..604800",    "IKE-SA-Lebensdauer in s (0 = Daemon-Vorgabe)", 0, 604800},
    {"childLifetimeS", Kind::Int,    "0..604800",    "Child-SA-Lebensdauer in s (0 = Daemon-Vorgabe)", 0, 604800},
    {"ikeEnc",         Kind::Algos,  "aes256cbc",    "IKE-Verschluesselung (Liste, Komma)", 0, 0},
    {"ikeHash",        Kind::Algos,  "sha256",       "IKE-Integritaet/PRF", 0, 0},
    {"ikeDh",          Kind::Algos,  "dh14",         "IKE-DH-Gruppe", 0, 0},
    {"espEnc",         Kind::Algos,  "aes256cbc",    "ESP-Verschluesselung", 0, 0},
    {"espHash",        Kind::Algos,  "sha256",       "ESP-Integritaet", 0, 0},
    {"auth",           Kind::Enum,   "psk|eap-mschapv2", "Authentifizierung", 0, 0},
    {"psk",            Kind::Secret, "",             "Pre-Shared Key, write-only ('ipsec psk' fragt verdeckt)", 0, 0},
    {"eapUser",        Kind::Str,    "",             "EAP-Benutzername (Identity, kein Secret)", 0, 0},
    {"eapPassword",    Kind::Secret, "",             "EAP-Passwort, write-only ('ipsec eap-password')", 0, 0},
    {"trustMode",      Kind::Enum,   "anchor-pem|host-store|host-store-plus-pem|none", "Zertifikatspruefung bei EAP", 0, 0},
    {"caPem",          Kind::Pem,    "",             "Trust-Anchor/CA als PEM ('ipsec ca-pem <datei>|-')", 0, 0},
    {"extraPem",       Kind::Pem,    "",             "zusaetzliches Kettenmaterial als PEM ('ipsec extra-pem <datei>|-')", 0, 0},
};

std::string lower(std::string s) { for (auto& ch : s) if (ch >= 'A' && ch <= 'Z') ch = (char)(ch - 'A' + 'a'); return s; }

// "remote-subnet", "remote_subnet", "RemoteSubnet" -> ein Schluessel.
std::string norm(const std::string& s) {
    std::string o;
    for (char ch : s) if (ch != '-' && ch != '_') o += (ch >= 'A' && ch <= 'Z') ? (char)(ch - 'A' + 'a') : ch;
    return o;
}

const Field* find_field(const std::string& given) {
    const std::string n = norm(given);
    for (const Field& f : kFields) if (norm(f.name) == n) return &f;
    return nullptr;
}

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) --b;
    return s.substr(a, b - a);
}

bool parse_bool(const std::string& v, bool& out) {
    const std::string l = lower(trim(v));
    for (const char* t : {"true", "1", "on", "yes", "ja", "an"})    if (l == t) { out = true;  return true; }
    for (const char* f : {"false", "0", "off", "no", "nein", "aus"}) if (l == f) { out = false; return true; }
    return false;
}

bool parse_int(const std::string& v, long long& out) {
    const std::string t = trim(v);
    if (t.empty()) return false;
    char* end = nullptr;
    errno = 0;
    const long long n = strtoll(t.c_str(), &end, 10);
    if (errno != 0 || !end || *end != '\0') return false;
    out = n;
    return true;
}

std::vector<std::string> split_on(const std::string& s, char sep) {
    std::vector<std::string> out;
    std::string cur;
    for (char ch : s) {
        if (ch == sep) { const std::string t = trim(cur); if (!t.empty()) out.push_back(t); cur.clear(); }
        else cur += ch;
    }
    const std::string t = trim(cur);
    if (!t.empty()) out.push_back(t);
    return out;
}

bool enum_allows(const char* allowed, const std::string& v) {
    for (const auto& a : split_on(allowed, '|')) if (a == v) return true;
    return false;
}

std::string scalar_str(const Json& j) {
    if (j.is_string()) return j.as_string();
    if (j.is_bool())   return j.as_bool() ? "true" : "false";
    if (j.is_null())   return "null";
    if (j.is_number()) {
        char b[64];
        if (j.is_integer()) snprintf(b, sizeof b, "%lld", j.as_int());
        else                snprintf(b, sizeof b, "%g", j.as_number());
        return b;
    }
    return j.dump();
}

void indent(std::string& out, int depth) { out.append((size_t)depth * 2, ' '); }

bool all_scalar(const Json& a) {
    for (size_t i = 0; i < a.size(); ++i) if (a.at(i).is_array() || a.at(i).is_object()) return false;
    return true;
}

void pretty_into(const Json& j, int depth, const std::string& key, std::string& out) {
    if (j.is_object()) {
        if (!key.empty()) { indent(out, depth); out += key; out += j.size() ? ":\n" : ": {}\n"; ++depth; }
        for (const auto& m : j.members()) pretty_into(m.second, depth, m.first, out);
        return;
    }
    if (j.is_array()) {
        if (j.size() == 0 || all_scalar(j)) {
            indent(out, depth);
            if (!key.empty()) { out += key; out += ": "; }
            if (j.size() == 0) out += "[]";
            for (size_t i = 0; i < j.size(); ++i) { if (i) out += ", "; out += scalar_str(j.at(i)); }
            out += "\n";
            return;
        }
        if (!key.empty()) { indent(out, depth); out += key; out += ":\n"; ++depth; }
        for (size_t i = 0; i < j.size(); ++i) {
            // Jedes Element eine Stufe tiefer rendern, dann den ersten Einzug
            // durch "- " ersetzen: Folgezeilen stehen buendig darunter.
            std::string item;
            pretty_into(j.at(i), depth + 1, "", item);
            const size_t pos = (size_t)depth * 2;
            if (item.size() >= pos + 2) item.replace(pos, 2, "- ");
            out += item;
        }
        return;
    }
    indent(out, depth);
    if (!key.empty()) { out += key; out += ": "; }
    out += scalar_str(j);
    out += "\n";
}

// ---- API-Aufruf mit lesbarer Fehlermeldung --------------------------------
struct Call {
    int         status = 0;
    bool        is_json = false;
    Json        json;
    std::string raw;
};

std::string api_error_text(const Call& r) {
    const std::string st = "HTTP " + std::to_string(r.status);
    if (!r.is_json)
        return st + " ohne JSON-Antwort -- antwortet auf diesem Port Machino? (streamerctl status)";
    const Json* e = r.json.get("error");
    if (e && e->is_object()) {
        const Json* code = e->get("code");
        const Json* msg  = e->get("message");
        std::string s = st;
        if (code && code->is_string()) s += " " + code->as_string();
        s += ": ";
        s += (msg && msg->is_string()) ? msg->as_string() : r.raw;
        return s;
    }
    return st + ": " + r.raw;
}

// require_json: eine 2xx-Antwort ohne JSON ist dann ein Fehler (die
// Machino-Routen antworten immer mit JSON; `api get /metrics` nicht).
bool call(Console& c, const char* method, const std::string& path, const std::string& body,
          Call& r, bool require_json = true) {
    HttpReply rep;
    std::string err;
    if (!c.http.request(method, path, body, rep, err)) { c.err("Fehler: " + err + "\n"); return false; }
    r.status = rep.status;
    r.raw = rep.body;
    std::string perr;
    r.is_json = Json::parse(rep.body, r.json, perr);
    if (rep.status >= 400) { c.err("Fehler: " + api_error_text(r) + "\n"); return false; }
    if (!r.is_json && require_json) {
        c.err("Fehler: die Antwort von " + path + " ist kein JSON (" + perr + ")\n");
        return false;
    }
    return true;
}

void show(Console& c, const Call& r) {
    if (c.raw_json || !r.is_json) {
        c.out(r.raw);
        if (r.raw.empty() || r.raw.back() != '\n') c.out("\n");
    } else {
        c.out(pretty(r.json));
    }
}

int get_and_show(Console& c, const std::string& path) {
    Call r;
    if (!call(c, "GET", path, "", r)) return 1;
    show(c, r);
    return 0;
}

const char* yn(bool b) { return b ? "ja" : "nein"; }

std::string str_of(const Json& j, const char* k, const char* dflt = "") {
    const Json* v = j.get(k);
    return (v && v->is_string()) ? v->as_string() : std::string(dflt);
}
bool bool_of(const Json& j, const char* k) { const Json* v = j.get(k); return v && v->is_bool() && v->as_bool(); }
bool has_bool(const Json& j, const char* k) { const Json* v = j.get(k); return v && v->is_bool(); }
long long int_of(const Json& j, const char* k) { const Json* v = j.get(k); return (v && v->is_number()) ? v->as_int() : 0; }

// ---- ipsec -----------------------------------------------------------------

// PUT + Ergebnis. Nennt die gespeicherten Felder (Secrets nur als gesetzt).
int ipsec_put(Console& c, const Json& body) {
    Call r;
    if (!call(c, "PUT", "/api/v1/ipsec/config", body.dump(), r)) return 1;
    if (c.raw_json) { show(c, r); return 0; }
    std::string s = "gespeichert:";
    for (const auto& m : body.members()) {
        const Field* f = find_field(m.first);
        s += " " + m.first + "=";
        s += (f && (f->kind == Kind::Secret || f->kind == Kind::Pem)) ? "(gesetzt)" : scalar_str(m.second);
    }
    c.out(s + "\n");
    c.out("Hinweis: wirkt auf die naechste Verbindung ('ipsec reconnect' bzw. 'ipsec connect').\n");
    return 0;
}

int ipsec_show_summary(Console& c) {
    Call cfg, st;
    if (!call(c, "GET", "/api/v1/ipsec", "", cfg)) return 1;
    if (!call(c, "GET", "/api/v1/ipsec/status", "", st)) return 1;
    if (c.raw_json) {
        Json j = Json::object();
        j.set("config", cfg.json);
        j.set("status", st.json);
        c.out(j.dump() + "\n");
        return 0;
    }
    c.out(ipsec_summary(cfg.json, st.json));
    return 0;
}

int ipsec_get(Console& c, const std::string& key) {
    const Field* f = find_field(key);
    if (!f) { c.err("unbekanntes Feld: " + key + " ('ipsec fields' zeigt alle)\n"); return 2; }
    Call cfg;
    if (!call(c, "GET", "/api/v1/ipsec", "", cfg)) return 1;
    if (f->kind == Kind::Secret || f->kind == Kind::Pem) {
        // Ein Secret kommt aus KEINER Antwort zurueck -- nur seine Anwesenheit.
        const bool set = bool_of(cfg.json, (std::string(f->name) + "Set").c_str());
        c.out(std::string(f->name) + ": write-only (gesetzt: " + yn(set) + ")\n");
        return 0;
    }
    const Json* v = cfg.json.get(f->name);
    if (!v) { c.err(std::string(f->name) + ": nicht in der Antwort\n"); return 1; }
    if (c.raw_json) { c.out(v->dump() + "\n"); return 0; }
    std::string s;
    if (v->is_array()) { for (size_t i = 0; i < v->size(); ++i) { if (i) s += ","; s += scalar_str(v->at(i)); } }
    else s = scalar_str(*v);
    c.out(std::string(f->name) + ": " + s + "\n");
    return 0;
}

int ipsec_secret(Console& c, const Field& f, const std::vector<std::string>& rest) {
    std::string v;
    if (rest.size() > 1) { c.err(std::string("erwartet: ipsec ") + (f.kind == Kind::Secret && !strcmp(f.name, "psk") ? "psk" : "eap-password") + " [<wert>]\n"); return 2; }
    if (rest.size() == 1) v = rest[0];
    else {
        if (!c.read_secret) { c.err("keine Eingabe moeglich -- Wert als Argument uebergeben\n"); return 2; }
        v = c.read_secret(std::string(f.name) + ": ");
    }
    if (v.empty()) { c.err(std::string(f.name) + ": leer -- nichts gespeichert (ein Secret laesst sich nur ersetzen)\n"); return 2; }
    Json body = Json::object();
    body.set(f.name, Json::string(v));
    Call r;
    if (!call(c, "PUT", "/api/v1/ipsec/config", body.dump(), r)) return 1;
    if (c.raw_json) { show(c, r); return 0; }
    c.out(std::string(f.name) + " gespeichert (write-only; 'ipsec reconnect' wendet ihn an).\n");
    return 0;
}

bool read_all_lines(Console& c, std::string& out) {
    if (!c.read_line) return false;
    std::string line;
    while (c.read_line(line)) { out += line; out += "\n"; }
    return true;
}

bool read_file(const std::string& path, std::string& out, std::string& err) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) { err = path + ": " + strerror(errno); return false; }
    char b[4096]; size_t n;
    while ((n = fread(b, 1, sizeof b, f)) > 0) out.append(b, n);
    fclose(f);
    return true;
}

int ipsec_pem(Console& c, const Field& f, const std::vector<std::string>& rest, const char* verb) {
    if (rest.size() != 1) { c.err(std::string("erwartet: ipsec ") + verb + " <datei>|-\n"); return 2; }
    std::string pem, err;
    if (rest[0] == "-") {
        if (!read_all_lines(c, pem)) { c.err("keine Eingabe moeglich\n"); return 2; }
    } else if (!read_file(rest[0], pem, err)) { c.err("Fehler: " + err + "\n"); return 1; }
    if (pem.find("-----BEGIN") == std::string::npos) { c.err(std::string(f.name) + ": das ist kein PEM (kein '-----BEGIN')\n"); return 2; }
    Json body = Json::object();
    body.set(f.name, Json::string(pem));
    Call r;
    if (!call(c, "PUT", "/api/v1/ipsec/config", body.dump(), r)) return 1;
    if (c.raw_json) { show(c, r); return 0; }
    c.out(std::string(f.name) + " gespeichert (" + std::to_string(pem.size()) + " Bytes).\n");
    return 0;
}

int ipsec_connect(Console& c) {
    Call r;
    if (!call(c, "POST", "/api/v1/ipsec/connect", "", r)) return 1;
    if (c.raw_json) { show(c, r); return 0; }
    Call st;
    if (!call(c, "GET", "/api/v1/ipsec/status", "", st)) return 1;
    c.out("Verbindung wird aufgebaut. Stand: " + str_of(st.json, "runtimeState", "?") + " (" + str_of(st.json, "state", "?") + ") -- 'ipsec' zeigt den Verlauf.\n");
    return 0;
}

int ipsec_disconnect(Console& c) {
    Call r;
    if (!call(c, "POST", "/api/v1/ipsec/disconnect", "", r)) return 1;
    if (c.raw_json) { show(c, r); return 0; }
    c.out("getrennt (kein Auto-Reconnect bis zum naechsten 'ipsec connect').\n");
    return 0;
}

// Frage mit Vorgabe: Enter uebernimmt [vorgabe], '-' leert das Feld.
// false = EOF (abgebrochen).
bool ask(Console& c, const std::string& label, const std::string& dflt, std::string& out) {
    c.out(label + " [" + dflt + "]: ");
    std::string line;
    if (!c.read_line(line)) { c.out("\n"); return false; }
    line = trim(line);
    if (line.empty())    out = dflt;
    else if (line == "-") out.clear();
    else                 out = line;
    return true;
}

int ipsec_setup(Console& c) {
    if (!c.read_line) { c.err("ipsec setup braucht eine Eingabe (Terminal)\n"); return 2; }
    Call cur;
    if (!call(c, "GET", "/api/v1/ipsec", "", cur)) return 1;
    const Json& k = cur.json;
    c.out("IPsec/VPN einrichten. Enter uebernimmt den Wert in [Klammern], '-' leert ein Feld, Strg-D bricht ab.\n");

    std::string v;
    Json body = Json::object();
    auto abort = [&]() { c.err("abgebrochen, nichts gespeichert.\n"); return 2; };

    if (!ask(c, "Gateway (Hostname oder IPv4)", str_of(k, "gateway"), v)) return abort();
    body.set("gateway", Json::string(v));
    for (;;) {
        if (!ask(c, "IKE-Port", std::to_string(int_of(k, "port") ? int_of(k, "port") : 500), v)) return abort();
        long long n; if (parse_int(v, n) && n >= 1 && n <= 65535) { body.set("port", Json::integer(n)); break; }
        c.out("  Ganzzahl 1..65535 erwartet.\n");
    }
    for (;;) {
        if (!ask(c, "Underlay auto|ethernet|wifi|cellular", str_of(k, "underlay", "auto"), v)) return abort();
        if (enum_allows("auto|ethernet|wifi|cellular", v)) { body.set("underlay", Json::string(v)); break; }
        c.out("  auto, ethernet, wifi oder cellular.\n");
    }
    if (!ask(c, "Lokale IKE-ID (z.B. cam.example.org)", str_of(k, "localId"), v)) return abort();
    body.set("localId", Json::string(v));
    if (!ask(c, "Remote-ID des Gateways", str_of(k, "remoteId"), v)) return abort();
    body.set("remoteId", Json::string(v));
    if (!ask(c, "Lokales Netz/Adresse, CIDR (z.B. 10.77.0.2/32)", str_of(k, "localSubnet"), v)) return abort();
    body.set("localSubnet", Json::string(v));
    if (!ask(c, "Remote-Netz, CIDR (z.B. 192.168.178.0/24)", str_of(k, "remoteSubnet"), v)) return abort();
    body.set("remoteSubnet", Json::string(v));

    std::string auth;
    for (;;) {
        if (!ask(c, "Authentifizierung psk|eap-mschapv2", str_of(k, "auth", "psk"), auth)) return abort();
        if (enum_allows("psk|eap-mschapv2", auth)) break;
        c.out("  psk oder eap-mschapv2.\n");
    }
    body.set("auth", Json::string(auth));
    if (auth == "psk") {
        const bool have = bool_of(k, "pskSet");
        std::string s = c.read_secret ? c.read_secret(std::string("PSK (leer = ") + (have ? "vorhandenen behalten" : "keiner, connect verweigert dann") + "): ") : std::string();
        if (!s.empty()) body.set("psk", Json::string(s));
    } else {
        if (!ask(c, "EAP-Benutzer", str_of(k, "eapUser"), v)) return abort();
        body.set("eapUser", Json::string(v));
        const bool have = bool_of(k, "eapPasswordSet");
        std::string s = c.read_secret ? c.read_secret(std::string("EAP-Passwort (leer = ") + (have ? "vorhandenes behalten" : "keins") + "): ") : std::string();
        if (!s.empty()) body.set("eapPassword", Json::string(s));
        for (;;) {
            if (!ask(c, "Zertifikatspruefung anchor-pem|host-store|host-store-plus-pem|none", str_of(k, "trustMode", "host-store"), v)) return abort();
            if (enum_allows("anchor-pem|host-store|host-store-plus-pem|none", v)) { body.set("trustMode", Json::string(v)); break; }
            c.out("  anchor-pem, host-store, host-store-plus-pem oder none.\n");
        }
        if ((v == "anchor-pem" || v == "host-store-plus-pem")) {
            const char* key = (v == "anchor-pem") ? "caPem" : "extraPem";
            const bool have = bool_of(k, (std::string(key) + "Set").c_str());
            std::string p;
            if (!ask(c, std::string(v == "anchor-pem" ? "CA-PEM-Datei" : "Extra-PEM-Datei") + (have ? " (leer = vorhandene behalten)" : ""), "", p)) return abort();
            if (!p.empty()) {
                std::string pem, err;
                if (!read_file(p, pem, err)) { c.err("Fehler: " + err + "\n"); return 1; }
                if (pem.find("-----BEGIN") == std::string::npos) { c.err(p + ": kein PEM\n"); return 2; }
                body.set(key, Json::string(pem));
            }
        }
    }
    bool b = false;
    for (;;) {
        if (!ask(c, "NAT-Traversal ja|nein", yn(!has_bool(k, "natT") || bool_of(k, "natT")), v)) return abort();
        if (parse_bool(v, b)) { body.set("natT", Json::boolean(b)); break; }
        c.out("  ja oder nein.\n");
    }
    for (;;) {
        if (!ask(c, "VPN aktivieren (enabled) ja|nein", "ja", v)) return abort();
        if (parse_bool(v, b)) { body.set("enabled", Json::boolean(b)); break; }
        c.out("  ja oder nein.\n");
    }

    const int rc = ipsec_put(c, body);
    if (rc != 0) return rc;
    if (!b) return 0;
    for (;;) {
        if (!ask(c, "Jetzt verbinden ja|nein", "ja", v)) return 0;
        bool yes;
        if (parse_bool(v, yes)) { return yes ? ipsec_connect(c) : 0; }
        c.out("  ja oder nein.\n");
    }
}

const char* kIpsecUsage =
    "ipsec [status|config|fields|get <k>|set <k> <v> ...|enable|disable|psk|eap-password|"
    "ca-pem <datei>|extra-pem <datei>|setup|connect|disconnect|reconnect]\n";

int cmd_ipsec(Console& c, const std::vector<std::string>& a) {
    if (a.empty()) return ipsec_show_summary(c);
    const std::string op = lower(a[0]);
    const std::vector<std::string> rest(a.begin() + 1, a.end());
    if (op == "status")  return get_and_show(c, "/api/v1/ipsec/status");
    if (op == "config")  return get_and_show(c, "/api/v1/ipsec");
    if (op == "fields")  { c.out(ipsec_fields_text()); return 0; }
    if (op == "get") {
        if (rest.size() != 1) { c.err("erwartet: ipsec get <schluessel>\n"); return 2; }
        return ipsec_get(c, rest[0]);
    }
    if (op == "set") {
        Json body; std::string err;
        if (!ipsec_set_body(rest, body, err)) { c.err(err + "\n"); return 2; }
        return ipsec_put(c, body);
    }
    if (op == "enable" || op == "disable") {
        Json body = Json::object();
        body.set("enabled", Json::boolean(op == "enable"));
        return ipsec_put(c, body);
    }
    if (op == "psk")          return ipsec_secret(c, *find_field("psk"), rest);
    if (op == "eap-password") return ipsec_secret(c, *find_field("eapPassword"), rest);
    if (op == "ca-pem")       return ipsec_pem(c, *find_field("caPem"), rest, "ca-pem");
    if (op == "extra-pem")    return ipsec_pem(c, *find_field("extraPem"), rest, "extra-pem");
    if (op == "connect")      return ipsec_connect(c);
    if (op == "disconnect")   return ipsec_disconnect(c);
    if (op == "reconnect") {
        // Trennen darf scheitern (nichts verbunden) -- der Aufbau zaehlt.
        Call r;
        if (call(c, "POST", "/api/v1/ipsec/disconnect", "", r) && !c.raw_json) c.out("getrennt, baue neu auf ...\n");
        return ipsec_connect(c);
    }
    if (op == "setup")        return ipsec_setup(c);
    c.err(std::string("unbekannt: ipsec ") + a[0] + "\n" + kIpsecUsage);
    return 2;
}

// ---- config ---------------------------------------------------------------

int cmd_config(Console& c, const std::vector<std::string>& a) {
    if (a.empty() || (lower(a[0]) == "get" && a.size() == 1)) return get_and_show(c, "/api/v1/config");
    const std::string op = lower(a[0]);
    if (op == "get") {
        if (a.size() != 2) { c.err("erwartet: config get [<pfad>]\n"); return 2; }
        Call r;
        if (!call(c, "GET", "/api/v1/config", "", r)) return 1;
        const Json* sub = json_path(r.json, a[1]);
        if (!sub) { c.err(a[1] + ": nicht vorhanden\n"); return 1; }
        if (c.raw_json) { c.out(sub->dump() + "\n"); return 0; }
        if (sub->is_object() || sub->is_array()) c.out(pretty(*sub));
        else c.out(a[1] + ": " + scalar_str(*sub) + "\n");
        return 0;
    }
    if (op == "set") {
        Json body; std::string err;
        if (!config_set_body(std::vector<std::string>(a.begin() + 1, a.end()), body, err)) { c.err(err + "\n"); return 2; }
        Call r;
        if (!call(c, "PATCH", "/api/v1/config", body.dump(), r)) return 1;
        show(c, r);
        return 0;
    }
    c.err("erwartet: config [get [<pfad>]] | config set <pfad>=<wert> ...\n");
    return 2;
}

// ---- api (Durchgriff) -----------------------------------------------------

// Objekte rekursiv zusammenfuehren (fuer config set a.b=1 a.c=2).
bool merge_into(Json& dst, const Json& src, std::string& err) {
    for (const auto& m : src.members()) {
        const Json* ex = dst.get(m.first);
        if (ex && m.second.is_object()) {
            if (!ex->is_object()) { err = m.first + " ist bereits ein Wert, kein Objekt"; return false; }
            Json child = *ex;
            if (!merge_into(child, m.second, err)) return false;
            dst.set(m.first, child);
        } else if (ex && ex->is_object()) {
            err = m.first + " ist bereits ein Objekt, kein Wert";
            return false;
        } else {
            dst.set(m.first, m.second);
        }
    }
    return true;
}

int cmd_api(Console& c, const std::vector<std::string>& a) {
    if (a.size() < 2) { c.err("erwartet: api get|put|post|patch|delete <pfad> [<json>|@datei|-]\n"); return 2; }
    std::string method = lower(a[0]);
    for (auto& ch : method) if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 'a' + 'A');
    if (method != "GET" && method != "PUT" && method != "POST" && method != "PATCH" && method != "DELETE") {
        c.err("Methode: get, put, post, patch oder delete\n"); return 2;
    }
    const std::string& path = a[1];
    if (path.empty() || path[0] != '/') { c.err("Pfad muss mit / beginnen, z.B. /api/v1/state\n"); return 2; }
    if (path == "/api/v1/events") { c.err("/api/v1/events ist ein Ereignisstrom (SSE) und endet nie -- dafuer curl -N verwenden\n"); return 2; }
    std::string body;
    if (a.size() >= 3) {
        if (a.size() > 3) { c.err("zu viele Argumente (den JSON-Body in Anfuehrungszeichen setzen)\n"); return 2; }
        const std::string& b = a[2];
        std::string err;
        if (b == "-") { if (!read_all_lines(c, body)) { c.err("keine Eingabe moeglich\n"); return 2; } }
        else if (!b.empty() && b[0] == '@') { if (!read_file(b.substr(1), body, err)) { c.err("Fehler: " + err + "\n"); return 1; } }
        else body = b;
        Json probe;
        if (!Json::parse(body, probe, err)) { c.err("Body ist kein JSON: " + err + "\n"); return 2; }
    }
    Call r;
    if (!call(c, method.c_str(), path, body, r, /*require_json=*/false)) return 1;
    show(c, r);
    return 0;
}

} // namespace

// ---- oeffentliche Helfer ---------------------------------------------------

std::vector<std::string> split_line(const std::string& line) {
    std::vector<std::string> out;
    std::string cur;
    bool in = false, dq = false, sq = false;
    for (size_t i = 0; i < line.size(); ++i) {
        const char ch = line[i];
        if (sq) { if (ch == '\'') sq = false; else cur += ch; continue; }
        if (dq) {
            if (ch == '"') { dq = false; continue; }
            if (ch == '\\' && i + 1 < line.size() && (line[i + 1] == '"' || line[i + 1] == '\\')) { cur += line[++i]; continue; }
            cur += ch; continue;
        }
        if (ch == '"')  { dq = true; in = true; continue; }
        if (ch == '\'') { sq = true; in = true; continue; }
        if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') {
            if (in) { out.push_back(cur); cur.clear(); in = false; }
            continue;
        }
        cur += ch; in = true;
    }
    if (in) out.push_back(cur);
    return out;
}

std::string ipsec_field_name(const std::string& given) {
    const Field* f = find_field(given);
    return f ? f->name : "";
}

bool ipsec_set_body(const std::vector<std::string>& kv, Json& body, std::string& err) {
    body = Json::object();
    if (kv.empty() || kv.size() % 2 != 0) {
        err = "erwartet: ipsec set <schluessel> <wert> [<schluessel> <wert> ...]  ('ipsec fields' zeigt die Schluessel)";
        return false;
    }
    for (size_t i = 0; i < kv.size(); i += 2) {
        const Field* f = find_field(kv[i]);
        const std::string& v = kv[i + 1];
        if (!f) { err = "unbekanntes Feld: " + kv[i] + " ('ipsec fields' zeigt alle)"; return false; }
        switch (f->kind) {
        case Kind::Bool: {
            bool b;
            if (!parse_bool(v, b)) { err = std::string(f->name) + ": true|false erwartet, nicht '" + v + "'"; return false; }
            body.set(f->name, Json::boolean(b));
            break;
        }
        case Kind::Int: {
            long long n;
            if (!parse_int(v, n)) { err = std::string(f->name) + ": Ganzzahl erwartet, nicht '" + v + "'"; return false; }
            if (n < f->lo || n > f->hi) { err = std::string(f->name) + ": " + std::to_string(n) + " liegt ausserhalb " + f->allowed; return false; }
            body.set(f->name, Json::integer(n));
            break;
        }
        case Kind::Str:
            body.set(f->name, Json::string(trim(v)));
            break;
        case Kind::Enum:
            if (!enum_allows(f->allowed, v)) { err = std::string(f->name) + ": erlaubt ist " + f->allowed + ", nicht '" + v + "'"; return false; }
            body.set(f->name, Json::string(v));
            break;
        case Kind::Algos: {
            Json arr = Json::array();
            for (const auto& s : split_on(v, ',')) arr.push(Json::string(s));
            if (arr.size() == 0) { err = std::string(f->name) + ": mindestens ein Algorithmus (erlaubt: " + f->allowed + ")"; return false; }
            body.set(f->name, arr);
            break;
        }
        case Kind::Secret:
            // Der Wert wird durchgereicht, taucht aber in keiner Meldung auf.
            if (v.empty()) { err = std::string(f->name) + ": leer (ein Secret laesst sich nur ersetzen, nicht loeschen)"; return false; }
            body.set(f->name, Json::string(v));
            break;
        case Kind::Pem:
            err = std::string(f->name) + ": PEM-Inhalt bitte ueber 'ipsec " + (f->name[0] == 'c' ? "ca-pem" : "extra-pem") + " <datei>|-' laden";
            return false;
        }
    }
    return true;
}

bool config_set_body(const std::vector<std::string>& assignments, Json& body, std::string& err) {
    body = Json::object();
    if (assignments.empty()) { err = "erwartet: config set <pfad>=<wert> [...]  (z.B. video.0.fps=15)"; return false; }
    for (const auto& a : assignments) {
        const size_t eq = a.find('=');
        if (eq == std::string::npos || eq == 0) { err = "erwartet <pfad>=<wert>, nicht '" + a + "'"; return false; }
        const std::vector<std::string> keys = split_on(a.substr(0, eq), '.');
        if (keys.empty()) { err = "leerer Pfad in '" + a + "'"; return false; }
        std::string v = a.substr(eq + 1);
        Json val;
        bool b; long long n;
        if (v.size() >= 2 && v.front() == '"' && v.back() == '"') val = Json::string(v.substr(1, v.size() - 2));
        else if (v == "null") val = Json::null();
        else if (v == "true" || v == "false") { parse_bool(v, b); val = Json::boolean(b); }
        else if (parse_int(v, n)) val = Json::integer(n);
        else {
            char* end = nullptr; errno = 0;
            const double d = strtod(v.c_str(), &end);
            if (!v.empty() && end && *end == '\0' && errno == 0) val = Json::number(d);
            else val = Json::string(v);
        }
        // Den Pfad von innen nach aussen zu einem Objekt falten und in den
        // Body mischen; ein Zwischenknoten, der schon ein Skalar ist, wird
        // MIT NAMEN abgelehnt.
        Json leaf = val;
        for (size_t i = keys.size(); i-- > 0;) { Json o = Json::object(); o.set(keys[i], leaf); leaf = o; }
        if (!merge_into(body, leaf, err)) return false;
    }
    return true;
}

std::string pretty(const Json& j) {
    if (j.is_object() && j.size() == 0) return "{}\n";
    std::string out;
    pretty_into(j, 0, "", out);
    return out;
}

std::string ipsec_summary(const Json& cfg, const Json& st) {
    std::string s;
    const std::string state = str_of(st, "state", "?");
    s += "ipsec    : enabled=" + std::string(yn(bool_of(cfg, "enabled"))) + "  state=" + state
       + "  runtime=" + str_of(st, "runtimeState", "?") + "  daemon=" + (bool_of(st, "daemonRunning") ? "laeuft" : "aus") + "\n";
    if (state == "failed") {
        const Json* f = st.get("failure");
        s += "FEHLER   : " + (f && f->is_object() ? str_of(*f, "code", "?") + " (notify " + std::to_string(int_of(*f, "lastNotify")) + ")" : std::string("?"));
        if (!str_of(st, "rawState").empty()) s += "  daemon=" + str_of(st, "rawState");
        s += "\n";
    }
    s += "gateway  : " + str_of(cfg, "gateway", "(keins)") + ":" + std::to_string(int_of(cfg, "port"));
    s += "  auth=" + str_of(cfg, "auth", "psk");
    if (str_of(cfg, "auth") == "eap-mschapv2") s += " (" + str_of(cfg, "eapUser", "?") + ", trust=" + str_of(cfg, "trustMode", "?") + ")";
    s += "\n";
    s += "underlay : " + str_of(cfg, "underlay", "auto");
    if (!str_of(st, "actualUnderlay").empty()) {
        s += " -> " + str_of(st, "actualUnderlay");
        if (!str_of(st, "underlayInterface").empty()) s += " (" + str_of(st, "underlayInterface") + ")";
    }
    s += "\n";
    s += "ids      : local=" + str_of(cfg, "localId", "(leer)") + "  remote=" + str_of(cfg, "remoteId", "(leer)") + "\n";
    s += "nets     : local=" + str_of(cfg, "localSubnet", "(leer)") + "  remote=" + str_of(cfg, "remoteSubnet", "(leer)");
    if (!str_of(st, "remoteTs").empty()) s += "  ausgehandelt=" + str_of(st, "remoteTs");
    s += "\n";
    const Json* routes = st.get("routes");
    s += "routes   : ";
    if (routes && routes->is_array() && routes->size() > 0) {
        for (size_t i = 0; i < routes->size(); ++i) {
            const Json& r = routes->at(i);
            if (i) s += ", ";
            s += str_of(r, "prefix", "?") + " (" + str_of(r, "source", "?") + ", " + str_of(r, "device", "?") + ")";
        }
    } else s += "(keine installiert)";
    if (bool_of(st, "fullTunnelRefused")) s += "  [0.0.0.0/0 vom Peer abgelehnt]";
    s += "\n";
    s += "transport: natT=" + std::string(yn(bool_of(cfg, "natT") || (!has_bool(cfg, "natT") && bool_of(st, "natT"))))
       + "  natDetected=" + yn(bool_of(st, "natDetected"));
    if (!str_of(st, "ikeTransport").empty()) s += "  ike=" + str_of(st, "ikeTransport");
    if (!str_of(st, "espTransport").empty()) s += "  esp=" + str_of(st, "espTransport");
    if (!str_of(st, "interface").empty())    s += "  if=" + str_of(st, "interface");
    s += "\n";
    s += "secrets  : psk=" + std::string(bool_of(cfg, "pskSet") ? "gesetzt" : "nicht gesetzt")
       + "  eapPassword=" + (bool_of(cfg, "eapPasswordSet") ? "gesetzt" : "nicht gesetzt")
       + "  caPem=" + (bool_of(cfg, "caPemSet") ? "gesetzt" : "nicht gesetzt") + "\n";
    if (bool_of(st, "daemonRunning")) {
        s += "traffic  : tx " + std::to_string(int_of(st, "txPackets")) + " Pakete/" + std::to_string(int_of(st, "txBytes")) + " B"
           + "  rx " + std::to_string(int_of(st, "rxPackets")) + " Pakete/" + std::to_string(int_of(st, "rxBytes")) + " B"
           + "  uptime " + std::to_string(int_of(st, "uptimeS")) + " s"
           + "  child-gen " + std::to_string(int_of(st, "childGeneration"))
           + "  ike-gen " + std::to_string(int_of(st, "ikeGeneration")) + "\n";
    }
    if (bool_of(st, "manualStop") || int_of(st, "reconnectAttempt") > 0) {
        s += "reconnect: ";
        if (bool_of(st, "manualStop")) s += "aus (manuell getrennt; 'ipsec connect' hebt das auf)";
        else s += "Versuch " + std::to_string(int_of(st, "reconnectAttempt")) + " geplant";
        s += "\n";
    }
    return s;
}

const char* help_text() {
    return
        "machinoctl -- Machino-Konsole (UART/SSH). Spricht mit machinod ueber die lokale API.\n"
        "\n"
        "Aufruf: machinoctl [--port N] [--json] <befehl> [argumente]\n"
        "        machinoctl                     interaktive Konsole ('exit' beendet)\n"
        "        --port N   API-Port (Vorgabe: 80, dann 8080; auch MACHINOCTL_PORT)\n"
        "        --json     Antworten unformatiert als JSON (fuer Skripte)\n"
        "\n"
        "Befehle:\n"
        "  help | ?                      diese Liste\n"
        "  status                        Zustand des Daemons (Lifecycle, Consumer, Medien)\n"
        "  telemetry                     Messwerte (FPS, Bitrate, CPU, RSS, ...)\n"
        "  config [get] [<pfad>]         Konfiguration zeigen (Pfad z.B. video.0)\n"
        "  config set <pfad>=<wert> ...  Konfiguration aendern (PATCH /api/v1/config), z.B. video.0.fps=15\n"
        "  ipsec                         VPN-Kurzansicht: Zustand, Gateway, Underlay, Netze, Routen, Fehler\n"
        "  ipsec status                  voller VPN-Status (/api/v1/ipsec/status)\n"
        "  ipsec config                  VPN-Konfiguration (Secrets nur als gesetzt/nicht gesetzt)\n"
        "  ipsec fields                  alle Schluessel mit Typ und erlaubten Werten\n"
        "  ipsec get <schluessel>        einen Wert lesen\n"
        "  ipsec set <schluessel> <wert> [<schluessel> <wert> ...]\n"
        "                                Werte speichern -- dieselbe Pruefung wie in der WebUI\n"
        "  ipsec enable | disable        Kurzform fuer set enabled true|false\n"
        "  ipsec psk [<wert>]            Pre-Shared Key (ohne Wert: verdeckte Eingabe; wird nie ausgegeben)\n"
        "  ipsec eap-password [<wert>]   EAP-Passwort (write-only wie der PSK)\n"
        "  ipsec ca-pem <datei>|-        CA/Trust-Anchor (PEM) aus Datei oder stdin\n"
        "  ipsec extra-pem <datei>|-     zusaetzliches Kettenmaterial (PEM)\n"
        "  ipsec setup                   gefuehrte Einrichtung: fragt ab, speichert, verbindet\n"
        "  ipsec connect | disconnect    Tunnel aufbauen / trennen (wie die Web-Buttons)\n"
        "  ipsec reconnect               trennen + neu aufbauen (nach einer Aenderung)\n"
        "  api get <pfad>                beliebige API-Route lesen, z.B. api get /api/v1/network\n"
        "  api put|post|patch|delete <pfad> [<json>|@datei|-]\n"
        "                                beliebige Route schreiben (Body als JSON)\n"
        "  exit | quit                   Konsole beenden\n"
        "\n"
        "Beispiel -- VPN zum Heimnetz ueber Mobilfunk:\n"
        "  ipsec set gateway vpn.example.org remoteSubnet 192.168.178.0/24 localSubnet 10.77.0.2/32 \\\n"
        "            localId cam.example.org remoteId vpn.example.org underlay cellular\n"
        "  ipsec psk\n"
        "  ipsec enable\n"
        "  ipsec connect\n";
}

std::string ipsec_fields_text() {
    std::string s = "Schluessel (ipsec set <schluessel> <wert>; Schreibweise egal: remoteSubnet = remote-subnet):\n";
    for (const Field& f : kFields) {
        char line[200];
        const char* type = f.kind == Kind::Bool ? "bool" : f.kind == Kind::Int ? "int" : f.kind == Kind::Str ? "text"
                         : f.kind == Kind::Enum ? "wahl" : f.kind == Kind::Algos ? "liste" : f.kind == Kind::Secret ? "secret" : "pem";
        snprintf(line, sizeof line, "  %-15s %-6s %-46s %s\n", f.name, type, f.allowed[0] ? f.allowed : "", f.help);
        s += line;
    }
    return s;
}

// ---- Antwort zerlegen -------------------------------------------------------

bool parse_http_reply(const std::string& raw, HttpReply& out, std::string& err) {
    if (raw.compare(0, 5, "HTTP/") != 0) { err = "keine HTTP-Antwort"; return false; }
    const size_t sp = raw.find(' ');
    if (sp == std::string::npos || sp + 4 > raw.size()) { err = "Statuszeile unvollstaendig"; return false; }
    out.status = atoi(raw.c_str() + sp + 1);
    if (out.status < 100 || out.status > 599) { err = "Statuszeile unlesbar"; return false; }
    size_t hdr_end = raw.find("\r\n\r\n");
    size_t skip = 4;
    if (hdr_end == std::string::npos) { hdr_end = raw.find("\n\n"); skip = 2; }
    if (hdr_end == std::string::npos) { err = "Header unvollstaendig"; return false; }
    // Content-Length (case-insensitiv) begrenzt den Body; ohne ihn zaehlt
    // alles bis zum Verbindungsende.
    const std::string head = lower(raw.substr(0, hdr_end));
    size_t clen = std::string::npos;
    const size_t cl = head.find("content-length:");
    if (cl != std::string::npos) {
        long long n;
        const size_t eol = head.find('\n', cl);
        if (parse_int(head.substr(cl + 15, eol == std::string::npos ? std::string::npos : eol - (cl + 15)), n) && n >= 0) clen = (size_t)n;
    }
    out.body = raw.substr(hdr_end + skip);
    if (clen != std::string::npos && out.body.size() > clen) out.body.resize(clen);
    return true;
}

// ---- Befehle / Konsole ------------------------------------------------------

int run_command(Console& c, const std::vector<std::string>& args) {
    if (args.empty()) { c.out(help_text()); return 2; }
    const std::string cmd = lower(args[0]);
    const std::vector<std::string> rest(args.begin() + 1, args.end());
    if (cmd == "help" || cmd == "?" || cmd == "--help" || cmd == "-h") { c.out(help_text()); return 0; }
    if (cmd == "status")    return get_and_show(c, "/api/v1/state");
    if (cmd == "telemetry") return get_and_show(c, "/api/v1/telemetry");
    if (cmd == "config")    return cmd_config(c, rest);
    if (cmd == "ipsec")     return cmd_ipsec(c, rest);
    if (cmd == "api")       return cmd_api(c, rest);
    c.err("unbekannter Befehl: " + args[0] + " ('help' zeigt die Liste)\n");
    return 2;
}

int run_repl(Console& c) {
    if (!c.read_line) { c.err("keine Eingabe\n"); return 2; }
    if (c.interactive) c.out("machinoctl -- Machino-Konsole. 'help' zeigt die Befehle, 'exit' beendet.\n");
    std::string line;
    for (;;) {
        if (c.interactive) c.out("machino> ");
        if (!c.read_line(line)) { if (c.interactive) c.out("\n"); return 0; }
        const std::vector<std::string> a = split_line(line);
        if (a.empty()) continue;
        const std::string w = lower(a[0]);
        if (w == "exit" || w == "quit" || w == "q") return 0;
        run_command(c, a);
    }
}

}} // namespace machino::ctl
