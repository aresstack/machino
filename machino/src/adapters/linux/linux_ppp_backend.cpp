#include "adapters/linux/linux_ppp_backend.hpp"

#include "core/log.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <sys/stat.h>
#include <unistd.h>

static const char* MOD = "ppp";

namespace machino { namespace linuxsys {

namespace {

std::string read_file(const std::string& path)
{
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return std::string();
    std::string s;
    char buf[1024];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) s.append(buf, n);
    fclose(f);
    return s;
}

// key=value je Zeile, wie bei der ECM-Zustandsdatei. "" wenn der Schluessel
// fehlt -- was etwas anderes ist als ein leerer Wert und hier gleich behandelt
// werden darf, weil beides "nichts brauchbares" heisst.
std::string field(const std::string& text, const std::string& key)
{
    const std::string k = key + "=";
    size_t p = 0;
    while (p < text.size()) {
        size_t e = text.find('\n', p);
        if (e == std::string::npos) e = text.size();
        if (text.compare(p, k.size(), k) == 0) {
            std::string v = text.substr(p + k.size(), e - p - k.size());
            while (!v.empty() && (v.back() == '\r' || v.back() == ' ')) v.pop_back();
            return v;
        }
        p = e + 1;
    }
    return std::string();
}

// Ein Feld, das gleich in eine Konfigurationsdatei geht, darf keine Zeile
// beenden und keinen Schalter erfinden koennen.
//
// Der APN kommt aus einem Formular. Ein Zeilenumbruch darin haenge eine
// beliebige pppd-Option an -- zum Beispiel "defaultroute", genau die, die hier
// unterdrueckt wird. Deshalb wird nicht maskiert, sondern abgewiesen: was
// hier nicht hineingehoert, hat auch keine sinnvolle maskierte Form.
bool plain(const std::string& s)
{
    for (char c : s)
        if (c == '\n' || c == '\r' || c == '"' || c == '\\' || (unsigned char)c < 0x20)
            return false;
    return true;
}

} // namespace

LinuxPppBackend::LinuxPppBackend(std::string conf_dir, std::string request_path,
                                 std::string status_path, std::string state_path)
    : conf_dir_(std::move(conf_dir)), request_path_(std::move(request_path)),
      status_path_(std::move(status_path)), state_path_(std::move(state_path)) {}

cellular::PppExit LinuxPppBackend::exit_from_code(int code)
{
    // Die Tabelle aus pppd(8). Nur die Faelle, die etwas Verschiedenes
    // BEDEUTEN -- der Rest ist "Other", und das ist ehrlicher, als jedem Code
    // einen eigenen Satz anzudichten.
    switch (code) {
        case 0:  return cellular::PppExit::Ok;
        case 5:  return cellular::PppExit::Ok;   // SIGTERM: wir haben es beendet
        case 8:  return cellular::PppExit::DialFailed;        // connect script failed
        case 10: return cellular::PppExit::NegotiationFailed; // PPP negotiation failed
        case 11: return cellular::PppExit::AuthFailed;        // peer failed to authenticate
        case 19: return cellular::PppExit::AuthFailed;        // we failed to authenticate
        case 15: return cellular::PppExit::LinkTerminated;    // peer not responding
        case 16: return cellular::PppExit::LinkTerminated;    // modem hung up
        default: return cellular::PppExit::Other;
    }
}

bool LinuxPppBackend::write_file(const std::string& path, const std::string& text, int mode) const
{
    const std::string tmp = path + ".machino-new";
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) { LOGW(MOD, "%s: %s", tmp.c_str(), strerror(errno)); return false; }
    // Die Rechte VOR dem Schreiben, nicht danach.
    //
    // Zwischen fopen und chmod liegt sonst ein Fenster, in dem die Datei mit
    // den Rechten der umask existiert -- und in diese Datei geht gleich ein
    // APN-Passwort. Ein Fenster von Mikrosekunden ist eines, das ein
    // Angreifer mit einer Schleife trifft.
    if (chmod(tmp.c_str(), mode) != 0)
        LOGW(MOD, "%s: chmod: %s", tmp.c_str(), strerror(errno));
    const bool ok = fwrite(text.data(), 1, text.size(), f) == text.size() &&
                    fflush(f) == 0;
    fclose(f);
    if (!ok) { unlink(tmp.c_str()); return false; }
    if (rename(tmp.c_str(), path.c_str()) != 0) {
        LOGW(MOD, "%s: rename: %s", path.c_str(), strerror(errno));
        unlink(tmp.c_str());
        return false;
    }
    return true;
}

Result LinuxPppBackend::start(const cellular::PppRequest& req)
{
    if (req.tty.empty()) return Result::error();
    if (!plain(req.tty) || !plain(req.apn) || !plain(req.dial) ||
        !plain(req.username) || !plain(req.password)) {
        // Welche Zeichen, aber nicht WO. Die Liste ist fest und verraet
        // nichts; den Wert zu zeigen hiesse, das Passwort ins Log zu
        // schreiben. Ohne die Liste weiss der Betreiber nur, dass etwas nicht
        // geht, und probiert dieselbe Eingabe noch einmal.
        LOGW(MOD, "the cellular settings contain a line break, a quote or a backslash - "
                  "pppd cannot take those in a configuration file, and nothing was written");
        return Result::error();
    }

    ::mkdir(conf_dir_.c_str(), 0700);

    // ---- das Chat-Skript --------------------------------------------------
    //
    // Die Escape-Sequenz steht HIER und nicht in machino: sie braucht eine
    // Sekunde Ruhe vor und nach "+++", sonst gehen die drei Zeichen als
    // Nutzdaten durch -- und sie gehoert auf den Port, den gleich pppd
    // uebernimmt. chat(8) kann beides; ein zweiter Sprecher auf demselben
    // seriellen Port waere der sicherste Weg, eine funktionierende Einwahl zu
    // zerlegen.
    //
    // ABORT vor allem anderen: ohne die Abbruchgruende wartet chat bei "BUSY"
    // oder "NO CARRIER" bis zum Timeout und meldet dann dasselbe wie bei einem
    // toten Port.
    std::string chat;
    chat += "ABORT 'BUSY'\n";
    chat += "ABORT 'NO CARRIER'\n";
    chat += "ABORT 'NO DIALTONE'\n";
    chat += "ABORT 'ERROR'\n";
    chat += "TIMEOUT 30\n";
    // Steckt der Port noch in einer alten Datensitzung, antwortet er auf nichts.
    // \\d ist eine Sekunde Pause fuer chat(8).
    chat += "'' '\\d\\d+++\\d\\d'\n";
    chat += "'' 'ATH'\n";
    chat += "'' 'AT'\n";
    chat += "OK 'ATD" + req.dial + "'\n";
    chat += "CONNECT ''\n";
    if (!write_file(conf_dir_ + "/chat", chat, 0600)) return Result::error();

    const bool need_auth = (req.auth != cellular::AuthMode::None) && !req.username.empty();

    // ---- die Optionen -----------------------------------------------------
    std::string o;
    o += req.tty + "\n";
    o += std::to_string(req.baud > 0 ? req.baud : 115200) + "\n";
    o += "lock\n";
    o += "crtscts\n";
    o += "modem\n";
    o += "noauth\n";              // das NETZ muss sich uns nicht ausweisen
    // KEIN persist, KEIN maxfail 0, KEIN holdoff.
    //
    // Naheliegend waere, pppd den Anruf selbst halten zu lassen. Das zerstoert
    // aber genau die Diagnose, fuer die es hier ueberhaupt einen Exit-Code
    // gibt: mit `persist` beendet sich pppd bei einer abgelehnten
    // Authentifizierung nicht, es waehlt wieder. Von aussen sieht das aus wie
    // "verhandelt noch" -- und nach zwoelf Sekunden meldet PppLink eine
    // haengende Aushandlung statt "Passwort falsch". Wer das einmal gesucht
    // hat, sucht es kein zweites Mal.
    //
    // Es gaebe ausserdem zwei Wiederholungslogiken uebereinander: pppds eigene
    // und den Backoff in PppLink. Die Wiederholung gehoert dorthin, wo auch
    // die SIM, die Registrierung und der PDP-Kontext geprueft werden -- pppd
    // kennt davon nichts und wuerde gegen eine nicht registrierte SIM
    // anwaehlen, bis jemand hinsieht.
    o += "maxfail 1\n";
    // KEINE Default-Route. core/net/route_plan entscheidet fuer alle Uplinks
    // zusammen, welche gewinnt -- ein pppd, der sich selbst eintraegt, nimmt
    // dem Betreiber die Wahl und dem Failover die Grundlage.
    o += "nodefaultroute\n";
    // usepeerdns SCHON, aber es schreibt nur /etc/ppp/resolv.conf -- nicht die
    // des Systems. Wir bekommen die Server damit zu sehen, ohne dass sie
    // jemandem aufgedraengt werden; welche gelten, sagt der aktive Uplink.
    o += "usepeerdns\n";
    o += "noipdefault\n";
    o += "connect '/usr/sbin/chat -v -f " + conf_dir_ + "/chat'\n";
    if (need_auth) {
        o += "user \"" + req.username + "\"\n";
        // Das Passwort steht HIER, in dieser 0600-Datei, und nicht in
        // /etc/ppp/pap-secrets.
        //
        // Es gibt keine pppd-Option, die den Pfad der Secrets-Dateien
        // verschiebt -- /etc/ppp/pap-secrets und /etc/ppp/chap-secrets sind
        // fest einkompiliert. Dorthin zu schreiben hiesse, eine Systemdatei zu
        // uebernehmen, die machino nicht gehoert und deren andere Zeilen
        // niemand kennt.
        //
        // `password` ist von pppd ausdruecklich NUR in einer Optionsdatei
        // erlaubt, nicht auf der Kommandozeile -- genau aus dem Grund, aus dem
        // es hier steht: in argv waere es in `ps` sichtbar.
        o += "password \"" + req.password + "\"\n";
        // Nur refuse-pap und refuse-chap, nicht refuse-mschap.
        //
        // MS-CHAP ist in diesem pppd nicht einkompiliert (CHAPMS=n): es ist ein
        // Microsoft-Einwahlverfahren, und Mobilfunk-APNs verlangen PAP oder
        // CHAP-MD5. Ein pppd, der eine Option nicht kennt, beendet sich mit
        // einem Optionsfehler -- die Einwahl scheiterte dann an der
        // Konfiguration, die sie absichern sollte.
        if (req.auth == cellular::AuthMode::Pap)  o += "refuse-chap\n";
        if (req.auth == cellular::AuthMode::Chap) o += "refuse-pap\n";
    }
    if (!write_file(conf_dir_ + "/options", o, 0600)) return Result::error();

    // ---- der Wunsch an den Helfer ----------------------------------------
    const std::string req_line = "ppp-start " + conf_dir_ + "/options\n";
    if (!write_file(request_path_, req_line, 0644)) return Result::error();
    LOGI(MOD, "pppd requested on %s", req.tty.c_str());
    return Result::ok();
}

Result LinuxPppBackend::stop()
{
    // EIN Wunsch, nicht zwei. Das Protokoll zum Helfer ist eine Zeile, die er
    // im Sekundentakt liest; zwei direkt hintereinander heissen, dass er den
    // ersten nie sieht. Derselbe Fehler ist im ECM-Pfad schon einmal passiert.
    if (!write_file(request_path_, std::string("ppp-stop\n"), 0644)) return Result::error();
    return Result::ok();
}

bool LinuxPppBackend::status(cellular::PppStatus& out) const
{
    const std::string st = read_file(status_path_);
    if (st.empty()) {
        // Nicht nachsehen koennen ist NICHT "nichts laeuft". Der Aufrufer muss
        // die beiden auseinanderhalten koennen, sonst erklaert er einen
        // laufenden pppd fuer tot und stellt einen zweiten daneben.
        return false;
    }

    out = cellular::PppStatus{};
    out.running = (field(st, "running") == "1");
    const std::string code = field(st, "exit");
    if (!code.empty()) out.exit = exit_from_code((int)strtol(code.c_str(), nullptr, 10));
    out.detail = field(st, "detail");

    // Interface und Adresse kommen aus der Zustandsdatei, die ip-up schreibt.
    // Dieselbe Datei wie beim ECM-Pfad, und das ist Absicht: es gibt EIN
    // Mobilfunkgeraet, und genau ein Datenlink ist zur Zeit aktiv.
    const std::string state = read_file(state_path_);
    if (!state.empty()) {
        const std::string iface = field(state, "iface");
        // NUR ein pppX. Die Datei teilen sich beide Datenpfade, und obwohl je
        // Boot nur einer laeuft, waere ein Rest des anderen hier still
        // wirksam: PPP meldete dann usb0 samt dessen Adresse als seine eigene,
        // und die Routen gingen ueber ein Interface, das dieser Anruf nie
        // angefasst hat. Der Name ist der einzige Hinweis, den wir haben --
        // also wird er geprueft.
        if (iface.compare(0, 3, "ppp") == 0) {
            out.ifname          = iface;
            out.address.ipv4    = field(state, "ipv4");
            out.address.gateway = field(state, "gateway");
            out.address.dns1    = field(state, "dns1");
            out.address.dns2    = field(state, "dns2");
        }
    }
    return true;
}

}} // namespace machino::linuxsys
