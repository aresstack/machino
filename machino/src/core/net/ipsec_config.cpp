#include "core/net/ipsec_config.hpp"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifndef _WIN32
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace machino { namespace ipsec {

namespace {

bool host_ok(const std::string& s)
{
    if (s.empty() || s.size() > 253) return false;
    for (char ch : s)
        if (!(isalnum((unsigned char)ch) || ch == '.' || ch == '-')) return false;
    return true;
}

bool id_ok(const std::string& s)
{
    if (s.size() > 63) return false;
    for (char ch : s)
        if (!(isalnum((unsigned char)ch) || ch == '.' || ch == '-' || ch == '_' || ch == '@'))
            return false;
    return true;
}

bool cidr_ok(const std::string& s)
{
    unsigned a, b, c, d, p;
    char extra;
    if (::sscanf(s.c_str(), "%u.%u.%u.%u/%u%c", &a, &b, &c, &d, &p, &extra) != 5) return false;
    return a < 256 && b < 256 && c < 256 && d < 256 && p <= 32;
}

// Die geschlossene AP2-Menge. Ein Eintrag ausserhalb wird MIT NAMEN abgelehnt.
std::string check_algos(const char* field, const std::vector<std::string>& got,
                        const char* only)
{
    if (got.size() != 1 || got[0] != only)
        return std::string(field) + ": erlaubt ist in dieser Stufe ausschliesslich '" + only
               + "' (AP2-bewiesene Suite); abgelehnt: '"
               + (got.empty() ? std::string("<leer>") : got[0])
               + "' — kein stilles Downgrade";
    return {};
}

std::string trim(const std::string& s)
{
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

} // namespace

const char* underlay_name(Underlay u)
{
    switch (u) {
        case Underlay::Auto: return "auto";
        case Underlay::Ethernet: return "ethernet";
        case Underlay::Wifi: return "wifi";
        case Underlay::Cellular: return "cellular";
    }
    return "auto";
}

bool underlay_from_name(const std::string& s, Underlay& out)
{
    if (s == "auto") out = Underlay::Auto;
    else if (s == "ethernet") out = Underlay::Ethernet;
    else if (s == "wifi") out = Underlay::Wifi;
    else if (s == "cellular") out = Underlay::Cellular;
    else return false;
    return true;
}

// Der PSK landet als "psk = <wert>"-Zeile in der Daemon-Datei und der
// Daemon-Parser TRIMMT den Wert. Beides erzwingt Regeln, die hier MIT
// BEGRUENDUNG durchgesetzt werden (die Meldung nennt NIE den Wert):
//  - kein \n/\r: waere eine Config-Zeilen-Injection in die Daemon-Datei;
//  - keine Leerzeichen am Rand: der Daemon saehe ein ANDERES Secret als
//    eingegeben -- ein unerklaerbarer Auth-Fail spaeter;
//  - 1..64 Bytes druckbares ASCII: die ENGSTE Grenze ist der WeirdIKE-Core
//    (WEIRDIKE_MAX_PSK=64, "reject oversized, no trunc") -- wd_config nimmt
//    zwar 128, aber weirdike_new() wiese den Wert dann beim START zurueck,
//    lange nach dem "gespeichert".
std::string psk_check(const std::string& psk)
{
    if (psk.empty()) return {};                     // leer = keinen neuen setzen
    if (psk.size() > 64) return "psk: laenger als 64 Bytes (WeirdIKE-Core-Limit)";
    for (char ch : psk) {
        if (ch == ' ') continue;                    // innen erlaubt, Rand unten
        if ((unsigned char)ch < 0x21 || (unsigned char)ch > 0x7e)
            return "psk: nur druckbares ASCII (keine Zeilenumbrueche/Steuerzeichen)";
    }
    if (psk.front() == ' ' || psk.back() == ' ')
        return "psk: fuehrende/abschliessende Leerzeichen wuerden vom Daemon entfernt -- abgelehnt statt still veraendert";
    return {};
}

std::string validate(const IpsecConfig& c)
{
    if (!c.gateway.empty() && !host_ok(c.gateway))
        return "gateway: unzulaessige Zeichen (erlaubt: Buchstaben, Ziffern, '.', '-')";
    if (c.enabled && c.gateway.empty())
        return "gateway: erforderlich, wenn enabled=true";
    if (c.port == 0)
        return "port: 1..65535";
    if (!c.local_id.empty() && !id_ok(c.local_id)) return "localId: unzulaessige Zeichen";
    if (!c.remote_id.empty() && !id_ok(c.remote_id)) return "remoteId: unzulaessige Zeichen";
    if (!c.local_subnet.empty() && !cidr_ok(c.local_subnet))
        return "localSubnet: kein gueltiges CIDR (a.b.c.d/n)";
    if (!c.remote_subnet.empty() && !cidr_ok(c.remote_subnet))
        return "remoteSubnet: kein gueltiges CIDR (a.b.c.d/n)";
    if (c.enabled && c.remote_subnet.empty())
        return "remoteSubnet: erforderlich (AP4-Scope: genau EIN Split-Netz)";
    if (c.dpd_interval_s < 0 || c.dpd_interval_s > 3600)
        return "dpdInterval: 0..3600";
    if (c.ike_lifetime_s > 86400u * 7) return "ikeLifetime: 0..604800";
    if (c.child_lifetime_s > 86400u * 7) return "childLifetime: 0..604800";

    std::string e;
    if (!(e = check_algos("ikeEnc", c.ike_enc, "aes256cbc")).empty()) return e;
    if (!(e = check_algos("ikeHash", c.ike_hash, "sha256")).empty()) return e;
    if (!(e = check_algos("ikeDh", c.ike_dh, "dh14")).empty()) return e;
    if (!(e = check_algos("espEnc", c.esp_enc, "aes256cbc")).empty()) return e;
    if (!(e = check_algos("espHash", c.esp_hash, "sha256")).empty()) return e;
    return {};
}

std::string to_machino_conf(const IpsecConfig& c)
{
    std::string s;
    s += "enabled = " + std::string(c.enabled ? "true" : "false") + "\n";
    s += "gateway = " + c.gateway + "\n";
    s += "port = " + std::to_string(c.port) + "\n";
    s += "underlay = " + std::string(underlay_name(c.underlay)) + "\n";
    s += "local_id = " + c.local_id + "\n";
    s += "remote_id = " + c.remote_id + "\n";
    s += "local_subnet = " + c.local_subnet + "\n";
    s += "remote_subnet = " + c.remote_subnet + "\n";
    s += "nat_t = " + std::string(c.nat_t ? "true" : "false") + "\n";
    s += "dpd_interval_s = " + std::to_string(c.dpd_interval_s) + "\n";
    s += "ike_lifetime_s = " + std::to_string(c.ike_lifetime_s) + "\n";
    s += "child_lifetime_s = " + std::to_string(c.child_lifetime_s) + "\n";
    s += "ike = aes256cbc,sha256,dh14\n";
    s += "esp = aes256cbc,sha256\n";
    return s;
}

bool from_machino_conf(const std::string& text, IpsecConfig& out, std::string& err)
{
    IpsecConfig c;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t eol = text.find('\n', pos);
        std::string line = trim(text.substr(pos, eol == std::string::npos ? std::string::npos
                                                                          : eol - pos));
        pos = eol == std::string::npos ? text.size() : eol + 1;
        if (line.empty() || line[0] == '#') continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos) { err = "Zeile ohne '=': " + line; return false; }
        std::string k = trim(line.substr(0, eq)), v = trim(line.substr(eq + 1));
        if (k == "enabled") c.enabled = (v == "true");
        else if (k == "gateway") c.gateway = v;
        else if (k == "port") c.port = (uint16_t)atoi(v.c_str());
        else if (k == "underlay") { if (!underlay_from_name(v, c.underlay)) { err = "underlay: '" + v + "'"; return false; } }
        else if (k == "local_id") c.local_id = v;
        else if (k == "remote_id") c.remote_id = v;
        else if (k == "local_subnet") c.local_subnet = v;
        else if (k == "remote_subnet") c.remote_subnet = v;
        else if (k == "nat_t") c.nat_t = (v == "true");
        else if (k == "dpd_interval_s") c.dpd_interval_s = atoi(v.c_str());
        else if (k == "ike_lifetime_s") c.ike_lifetime_s = (uint32_t)strtoul(v.c_str(), nullptr, 10);
        else if (k == "child_lifetime_s") c.child_lifetime_s = (uint32_t)strtoul(v.c_str(), nullptr, 10);
        else if (k == "ike" || k == "esp") { /* informativ; die Menge ist geschlossen */ }
        else { err = "unbekannter Schluessel: " + k; return false; }
    }
    const std::string ve = validate(c);
    if (!ve.empty() && c.enabled) { err = ve; return false; }
    out = c;
    return true;
}

std::string to_weirdike_conf(const IpsecConfig& c, const std::string& psk,
                             const std::string& old_daemon_conf, bool* psk_present)
{
    // PSK write-only: neuer Wert gewinnt; sonst den alten Zeilenwert
    // unveraendert weitertragen (nur der Daemon liest ihn je wieder).
    std::string psk_line;
    if (!psk.empty()) {
        psk_line = "psk = " + psk + "\n";
    } else {
        size_t pos = 0;
        while (pos < old_daemon_conf.size()) {
            size_t eol = old_daemon_conf.find('\n', pos);
            std::string line = old_daemon_conf.substr(
                pos, eol == std::string::npos ? std::string::npos : eol - pos);
            pos = eol == std::string::npos ? old_daemon_conf.size() : eol + 1;
            std::string t = trim(line);
            if (t.rfind("psk", 0) == 0 && t.find('=') != std::string::npos) {
                psk_line = t + "\n";
                break;
            }
        }
    }
    if (psk_present) *psk_present = !psk_line.empty();

    std::string s;
    s += "gateway = " + c.gateway + "\n";
    s += "port = " + std::to_string(c.port) + "\n";
    s += psk_line;
    if (!c.local_id.empty())  s += "local_id = " + c.local_id + "\n";
    if (!c.remote_id.empty()) s += "remote_id = " + c.remote_id + "\n";
    if (!c.local_subnet.empty())  s += "local_subnet = " + c.local_subnet + "\n";
    if (!c.remote_subnet.empty()) s += "remote_subnet = " + c.remote_subnet + "\n";
    s += "nat_t = " + std::string(c.nat_t ? "true" : "false") + "\n";
    s += "dpd_interval_s = " + std::to_string(c.dpd_interval_s) + "\n";
    if (c.ike_lifetime_s)   s += "ike_lifetime_s = " + std::to_string(c.ike_lifetime_s) + "\n";
    if (c.child_lifetime_s) s += "child_lifetime_s = " + std::to_string(c.child_lifetime_s) + "\n";
    return s;
}

bool write_atomic_0600(const std::string& path, const std::string& content, std::string& err)
{
    const std::string tmp = path + ".machino-new";
#ifndef _WIN32
    // Die Datei traegt ein Secret: von der ERSTEN Millisekunde an 0600.
    // fopen+chmod-danach liesse ein umask-Fenster (typisch 0644), in dem
    // jeder lokale Leser den PSK saehe. O_EXCL raeumt zugleich einen
    // liegengebliebenen tmp-Rest eines abgebrochenen Laufs beiseite.
    ::remove(tmp.c_str());
    const int fdo = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fdo < 0) { err = "kann " + tmp + " nicht anlegen"; return false; }
    FILE* f = ::fdopen(fdo, "wb");
    if (!f) { ::close(fdo); ::remove(tmp.c_str()); err = "fdopen fehlgeschlagen"; return false; }
#else
    FILE* f = ::fopen(tmp.c_str(), "wb");
    if (!f) { err = "kann " + tmp + " nicht schreiben"; return false; }
#endif
    const bool wrote = ::fwrite(content.data(), 1, content.size(), f) == content.size();
#ifndef _WIN32
    if (wrote) ::fflush(f), ::fsync(::fileno(f));
#endif
    if (::fclose(f) != 0 || !wrote) { ::remove(tmp.c_str()); err = "Schreiben unvollstaendig"; return false; }
#ifdef _WIN32
    // NUR Windows: rename ersetzt dort kein Ziel. Auf POSIX bliebe nach einem
    // remove ein Fenster ganz OHNE Konfigurationsdatei -- exakt die halbe
    // VPN-Config, die dieses Modul verhindern soll; dort ersetzt rename atomar.
    ::remove(path.c_str());
#endif
    if (::rename(tmp.c_str(), path.c_str()) != 0) {
        ::remove(tmp.c_str());
        err = "rename fehlgeschlagen";
        return false;
    }
    return true;
}

}} // namespace machino::ipsec
