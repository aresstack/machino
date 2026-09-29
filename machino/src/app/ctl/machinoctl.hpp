// machinoctl: die Terminal-Bedienung von Machino (UART / SSH).
//
// WeirdOS (esp32-modem-host/serial_console.cpp) bedient das Geraet ueber eine
// serielle Kommandokonsole, weil ein ESP32 keine Shell hat. Auf der Kamera
// gibt es die Shell schon -- UART und dropbear/SSH landen in busybox ash --
// also ist das Machino-Gegenstueck ein Kommando, das man dort eintippt:
//
//   machinoctl ipsec set gateway vpn.example.org remoteSubnet 192.168.178.0/24
//   machinoctl ipsec psk                # verdeckte Eingabe
//   machinoctl ipsec connect
//   machinoctl                          # ohne Argumente: Konsole wie bei WeirdOS
//
// Es ist ein MODUS des machino-Binaries (`machino --ctl ...`, Wrapper
// /usr/sbin/machinoctl): kein zweites Binary auf dem 4,6-MB-Overlay, und der
// JSON-Code ist derselbe wie im Daemon. Gesprochen wird AUSSCHLIESSLICH ueber
// die vorhandene HTTP-API auf 127.0.0.1 -- kamera-lokale Aufrufer sind dort
// ohne Anmeldung vertrauenswuerdig (SessionGate::is_local_peer, wie bei
// Majestic). Kein zweiter Konfigurationspfad, keine Datei wird an machinod
// vorbei geschrieben: was die WebUI kann, kann die Konsole, und beide gehen
// durch dieselbe Validierung (Ablehnung MIT NAMEN, write-only-Secrets).
//
// Alles ausser dem Socket ist rein und hosttestbar: die Konsole nimmt einen
// IHttpClient, eine Zeilenquelle und eine Secret-Quelle.
#pragma once
#include "core/json.hpp"

#include <functional>
#include <string>
#include <vector>

namespace machino { namespace ctl {

struct HttpReply {
    int         status = 0;
    std::string body;
};

class IHttpClient {
public:
    virtual ~IHttpClient() = default;
    // false + err = machinod nicht erreichbar (kein HTTP-Status). Ein
    // HTTP-Fehlerstatus ist KEIN Transportfehler: true, out.status >= 400.
    virtual bool request(const std::string& method, const std::string& path,
                         const std::string& body, HttpReply& out, std::string& err) = 0;
};

// Der echte Client: TCP nach 127.0.0.1, blockierend, `Connection: close`.
// Probiert die Ports der Reihe nach (Front-Door 80, sonst api.port 8080),
// bis einer eine Verbindung annimmt.
class LoopbackHttpClient : public IHttpClient {
public:
    explicit LoopbackHttpClient(std::vector<int> ports) : ports_(std::move(ports)) {}
    bool request(const std::string& method, const std::string& path,
                 const std::string& body, HttpReply& out, std::string& err) override;
    int  used_port() const { return used_; }
private:
    std::vector<int> ports_;
    int              used_ = 0;
};

// Antwort eines Servers zerlegen (Statuszeile + Header + Body, Content-Length
// wird respektiert). Exponiert fuer die Tests.
bool parse_http_reply(const std::string& raw, HttpReply& out, std::string& err);

struct Console {
    IHttpClient& http;
    std::function<void(const std::string&)>           out;          // stdout
    std::function<void(const std::string&)>           err;          // stderr
    std::function<bool(std::string&)>                 read_line;    // false = EOF
    std::function<std::string(const std::string&)>    read_secret;  // verdeckt; "" = nichts
    // AP11: ein Kommando der Kamera-Shell ausfuehren (ping, curl, logread)
    // und seine Ausgabe liefern -- fuer die Tunnel-Diagnose. machinoctl ist
    // ein eigener Prozess in der Shell des Bedienenden; die "machinod forkt
    // nicht"-Regel ist davon unberuehrt. Null = nicht verfuegbar (Hosttests).
    std::function<bool(const std::string& cmdline, std::string& out)> shell;
    bool raw_json = false;   // --json: Antworten unformatiert (fuer Skripte)
    bool interactive = true; // Konsole: Banner + Prompt (false bei Pipe/Skript)
};

// 0 = ok, 1 = API-/Transportfehler, 2 = Bedienfehler (Hilfe gezeigt).
int run_command(Console& c, const std::vector<std::string>& args);
// Die Konsole: liest Zeilen bis EOF / exit. Rueckgabe wie run_command des
// letzten Befehls ist NICHT sinnvoll -- die Konsole gibt 0 zurueck.
int run_repl(Console& c);
// Einstieg aus main(): `machino --ctl [--port N] [--json] [befehl ...]`.
int run_ctl(int argc, char** argv, int first);

// ---- reine Helfer (hosttestbar) -------------------------------------------

// Eine Konsolenzeile in Argumente: Leerraum trennt, "..." und '...' halten
// zusammen, \ maskiert innerhalb von "...".
std::vector<std::string> split_line(const std::string& line);

// `ipsec set key wert [key wert ...]` -> PUT-Body. Schluessel werden
// unabhaengig von Schreibweise erkannt (remoteSubnet, remote-subnet,
// remote_subnet), Werte typisiert (bool/int/liste), Secrets/PEM nur ueber
// die dafuer vorgesehenen Wege. Fehler nennt den Schluessel, nie einen
// Secret-Wert.
bool ipsec_set_body(const std::vector<std::string>& kv, Json& body, std::string& err);
// Der kanonische API-Name eines Feldes ("" = unbekannt).
std::string ipsec_field_name(const std::string& given);

// `config set a.b=c ...` -> PATCH-Body {"a":{"b":c}} (true/false/Zahl/null
// typisiert, "..." erzwingt String).
bool config_set_body(const std::vector<std::string>& assignments, Json& body, std::string& err);

// key: value, verschachtelt eingerueckt; Listen aus Skalaren in einer Zeile.
std::string pretty(const Json& j);
// Die Kurzansicht von `ipsec` (Config ohne Secrets + Status), wie WeirdOS'
// `ipsec`: Zustand, Gateway, Underlay, IDs, Netze, Routen, Fehlerklasse.
std::string ipsec_summary(const Json& cfg, const Json& status);
// Die Befehlsliste ('help').
const char* help_text();
// AP11: der Algorithmen-Katalog aus GET /api/v1/ipsec (algorithms) als Text.
std::string ipsec_algos_text(const Json& catalogue, const Json& cfg);
// AP11: Shell-Kommandozeilen der Diagnose (rein; Argumente werden geprueft).
bool ping_cmdline(const std::string& target, std::string& cmd, std::string& err);
bool fetch_cmdline(const std::string& target, std::string& cmd, std::string& err);
std::string log_cmdline(int lines);
// Die Feldtabelle ('ipsec fields').
std::string ipsec_fields_text();

}} // namespace machino::ctl
