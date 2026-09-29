// machinoctl (Terminal-Bedienung, UART/SSH): die reinen Helfer, die Befehle
// gegen einen Fake-HTTP-Client und -- als Vertragsbeweis -- gegen die ECHTE
// ApiService mit Fake-IPsec-Backend: jedes Feld, das die Konsole kennt, muss
// PUT /api/v1/ipsec/config auch annehmen, und kein Secret darf je in einer
// Ausgabe oder Fehlermeldung der Konsole stehen.
#include "app/ctl/machinoctl.hpp"
#include "app/api/api_service.hpp"
#include "core/config_store.hpp"
#include "core/detection/detection_service.hpp"
#include "core/events.hpp"
#include "core/hw/registry.hpp"
#include "core/hw/resolve.hpp"
#include "core/lifecycle/pipeline_manager.hpp"
#include "core/media/tuning_service.hpp"
#include "core/net/ipsec_service.hpp"
#include "core/power/performance_service.hpp"
#include "core/stream_hub.hpp"
#include "fake_platform.hpp"
#include "fake_power.hpp"

#include <cstdio>
#include <map>
#include <string>
#include <vector>

using namespace machino;
using namespace machino::ctl;

extern int g_fail_ext, g_pass_ext;
#define CCHECK(cond) do { if (cond) { ++g_pass_ext; } else { ++g_fail_ext; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

namespace {

bool has(const std::string& hay, const char* needle) { return hay.find(needle) != std::string::npos; }

// Zeichnet jeden Request auf und antwortet aus einer Tabelle "METHOD path".
struct FakeHttp : IHttpClient {
    struct Req { std::string method, path, body; };
    std::vector<Req> reqs;
    std::map<std::string, HttpReply> replies;
    bool down = false;

    void reply(const char* method, const char* path, int status, const std::string& body) {
        replies[std::string(method) + " " + path] = HttpReply{status, body};
    }
    bool request(const std::string& method, const std::string& path, const std::string& body,
                 HttpReply& out, std::string& err) override {
        if (down) { err = "machinod nicht erreichbar auf 127.0.0.1:80/8080 (80: Connection refused) -- laeuft Machino? (streamerctl status)"; return false; }
        reqs.push_back({method, path, body});
        auto it = replies.find(method + " " + path);
        if (it == replies.end()) { out = HttpReply{404, "{\"ok\":false,\"error\":{\"code\":\"not_found\",\"message\":\"kein Fake fuer " + method + " " + path + "\"}}"}; return true; }
        out = it->second;
        return true;
    }
};

// Eine Konsole mit Skript-Eingabe und aufgefangener Ausgabe.
struct Term {
    FakeHttp http;
    std::string out, err;
    std::vector<std::string> lines;
    size_t next = 0;
    std::vector<std::string> secrets;
    size_t next_secret = 0;
    std::vector<std::string> prompts;   // was read_secret gefragt hat
    std::vector<std::string> shell_cmds; // AP11: was die Konsole ausfuehren wollte
    std::string shell_out = "ok\n"; bool shell_ok = true;
    Console c;

    Term() : c{http,
               [this](const std::string& s) { out += s; },
               [this](const std::string& s) { err += s; },
               [this](std::string& l) { if (next >= lines.size()) return false; l = lines[next++]; return true; },
               [this](const std::string& p) { prompts.push_back(p); return next_secret < secrets.size() ? secrets[next_secret++] : std::string(); },
               [this](const std::string& cmd, std::string& o) { shell_cmds.push_back(cmd); o = shell_out; return shell_ok; },
               false, true} {}
    int run(const std::vector<std::string>& a) { return run_command(c, a); }
    const FakeHttp::Req& last() const { return http.reqs.back(); }
};

const char* CFG_JSON =
    "{\"enabled\":true,\"gateway\":\"vpn.example.org\",\"port\":500,\"underlay\":\"cellular\","
    "\"localId\":\"cam.test\",\"remoteId\":\"vpn.test\",\"localSubnet\":\"10.77.0.2/32\","
    "\"remoteSubnet\":\"10.66.0.0/24\",\"natT\":true,\"dpdIntervalS\":30,\"ikeLifetimeS\":0,"
    "\"childLifetimeS\":0,\"ikeEnc\":[\"aes256cbc\"],\"ikeHash\":[\"sha256\"],\"ikeDh\":[\"dh14\"],"
    "\"espEnc\":[\"aes256cbc\"],\"espHash\":[\"sha256\"],\"pskSet\":true,\"auth\":\"psk\","
    "\"eapUser\":\"\",\"trustMode\":\"host-store\",\"eapPasswordSet\":false,\"caPemSet\":false,"
    "\"extraPemSet\":false,\"hostStoreAvailable\":true,"
    "\"localIdType\":\"fqdn\",\"remoteIdType\":\"fqdn\",\"pfs\":false,\"autoConnect\":true,\"requestCp\":false,"
    "\"dpd\":true,\"dpdRetries\":0,\"nattKeepaliveS\":0,\"childLifetimeMb\":0,\"mtu\":1400,"
    "\"algorithms\":{\"ikeDh\":[{\"id\":\"dh2\",\"label\":\"DH2 - MODP-1024\",\"implemented\":false,\"lancomDefault\":false},"
    "{\"id\":\"dh14\",\"label\":\"DH14 - MODP-2048\",\"implemented\":true,\"lancomDefault\":true},"
    "{\"id\":\"dh19\",\"label\":\"DH19 - ECP-256\",\"implemented\":true,\"lancomDefault\":false}],"
    "\"ikeEnc\":[{\"id\":\"aes256cbc\",\"label\":\"AES-CBC-256\",\"implemented\":true,\"lancomDefault\":true}],"
    "\"ikeHash\":[{\"id\":\"sha1\",\"label\":\"SHA-1\",\"implemented\":true,\"lancomDefault\":true},"
    "{\"id\":\"sha256\",\"label\":\"SHA-256\",\"implemented\":true,\"lancomDefault\":true}],"
    "\"espEnc\":[{\"id\":\"aes256cbc\",\"label\":\"AES-CBC-256\",\"implemented\":true,\"lancomDefault\":true}],"
    "\"espHash\":[{\"id\":\"sha256\",\"label\":\"SHA-256\",\"implemented\":true,\"lancomDefault\":true}]}}";

const char* STATUS_JSON =
    "{\"daemonRunning\":true,\"state\":\"childEstablished\",\"runtimeState\":\"dataPlaneUp\","
    "\"manualStop\":false,\"rawState\":\"CHILD_SA_ESTABLISHED\",\"gateway\":\"203.0.113.5:4500\","
    "\"interface\":\"ipsec0\",\"remoteTs\":\"10.66.0.0/24\",\"natT\":true,\"natDetected\":true,"
    "\"requestedUnderlay\":\"cellular\",\"actualUnderlay\":\"cellular\",\"underlayInterface\":\"usb0\","
    "\"ikeTransport\":\"udp4500\","
    "\"espTransport\":\"udp4500\",\"auth\":\"psk\","
    "\"routes\":[{\"prefix\":\"10.66.0.0/24\",\"source\":\"tsr\",\"device\":\"ipsec0\"}],"
    "\"childGeneration\":1,\"ikeGeneration\":1,\"uptimeS\":120,\"txPackets\":12,\"txBytes\":1024,"
    "\"rxPackets\":10,\"rxBytes\":900,\"tunnelIpv4\":\"10.77.0.2\",\"requestCp\":false,\"pfsGroup\":0,"
    "\"ikeSuite\":\"aes256cbc/sha256/sha256/dh14\",\"childSuite\":\"aes256cbc/sha256\"}";

void test_split_line() {
    std::vector<std::string> a = split_line("  ipsec set  gateway \"vpn example\" 'it''s' a\\b \"q\\\"x\"\t");
    CCHECK(a.size() == 7);
    CCHECK(a[0] == "ipsec" && a[1] == "set" && a[2] == "gateway");
    CCHECK(a[3] == "vpn example");
    CCHECK(a[4] == "its");                 // zwei angrenzende '...' verschmelzen
    CCHECK(a[5] == "a\\b");                // Backslash ausserhalb von "..." bleibt
    CCHECK(a[6] == "q\"x");                // \" innerhalb von "..."
    CCHECK(split_line("").empty() && split_line("   \t ").empty());
    CCHECK(split_line("\"\"").size() == 1 && split_line("\"\"")[0].empty());   // leeres Argument bleibt eines
}

void test_field_names() {
    CCHECK(ipsec_field_name("remoteSubnet") == "remoteSubnet");
    CCHECK(ipsec_field_name("remote-subnet") == "remoteSubnet");
    CCHECK(ipsec_field_name("REMOTE_SUBNET") == "remoteSubnet");
    CCHECK(ipsec_field_name("natt") == "natT");
    CCHECK(ipsec_field_name("nat-t") == "natT");
    CCHECK(ipsec_field_name("gateay").empty());
    CCHECK(ipsec_field_name("").empty());
}

void test_ipsec_set_body() {
    Json b; std::string err;
    CCHECK(ipsec_set_body({"gateway", " vpn.example.org ", "port", "4500", "nat-t", "off", "ikeEnc", "aes256cbc, aes256cbc",
                           "underlay", "cellular", "dpdIntervalS", "0", "psk", "s3cret!"}, b, err));
    CCHECK(err.empty());
    CCHECK(b.get("gateway") && b.get("gateway")->as_string() == "vpn.example.org");   // getrimmt
    CCHECK(b.get("port") && b.get("port")->is_integer() && b.get("port")->as_int() == 4500);
    CCHECK(b.get("natT") && b.get("natT")->is_bool() && !b.get("natT")->as_bool());
    CCHECK(b.get("ikeEnc") && b.get("ikeEnc")->is_array() && b.get("ikeEnc")->size() == 2);
    CCHECK(b.get("underlay") && b.get("underlay")->as_string() == "cellular");
    CCHECK(b.get("dpdIntervalS") && b.get("dpdIntervalS")->as_int() == 0);
    CCHECK(b.get("psk") && b.get("psk")->as_string() == "s3cret!");
    CCHECK(b.size() == 7);

    // Ablehnung MIT NAMEN, nie mit einem Secret-Wert.
    CCHECK(!ipsec_set_body({"gateay", "x"}, b, err) && has(err, "gateay") && has(err, "ipsec fields"));
    CCHECK(!ipsec_set_body({"gateway"}, b, err) && has(err, "ipsec set <schluessel> <wert>"));
    CCHECK(!ipsec_set_body({}, b, err));
    CCHECK(!ipsec_set_body({"enabled", "maybe"}, b, err) && has(err, "enabled") && has(err, "true|false"));
    CCHECK(!ipsec_set_body({"port", "70000"}, b, err) && has(err, "port") && has(err, "1..65535"));
    CCHECK(!ipsec_set_body({"port", "abc"}, b, err) && has(err, "Ganzzahl"));
    CCHECK(!ipsec_set_body({"underlay", "lte"}, b, err) && has(err, "auto|ethernet|wifi|cellular") && has(err, "lte"));
    CCHECK(!ipsec_set_body({"auth", "cert"}, b, err) && has(err, "psk|eap-mschapv2"));
    CCHECK(!ipsec_set_body({"ikeEnc", " , "}, b, err) && has(err, "ikeEnc"));
    CCHECK(!ipsec_set_body({"psk", ""}, b, err) && has(err, "psk") && has(err, "leer"));
    CCHECK(!ipsec_set_body({"caPem", "-----BEGIN"}, b, err) && has(err, "ipsec ca-pem"));
    CCHECK(!ipsec_set_body({"extra-pem", "x"}, b, err) && has(err, "ipsec extra-pem"));
    // Sprachvarianten fuer bool
    CCHECK(ipsec_set_body({"enabled", "ja"}, b, err) && b.get("enabled")->as_bool());
    CCHECK(ipsec_set_body({"enabled", "nein"}, b, err) && !b.get("enabled")->as_bool());
    // letzter Wert gewinnt bei Wiederholung
    CCHECK(ipsec_set_body({"port", "500", "port", "4500"}, b, err) && b.size() == 1 && b.get("port")->as_int() == 4500);
}

void test_config_set_body() {
    Json b; std::string err;
    CCHECK(config_set_body({"video.0.fps=15", "video.0.bitrate_kbps=1500", "sensor.fps=\"20\"", "ai.enabled=true", "x.y=null", "s=hello world", "f=1.5"}, b, err));
    CCHECK(b.dump() == "{\"video\":{\"0\":{\"fps\":15,\"bitrate_kbps\":1500}},\"sensor\":{\"fps\":\"20\"},\"ai\":{\"enabled\":true},\"x\":{\"y\":null},\"s\":\"hello world\",\"f\":1.5}");
    CCHECK(!config_set_body({}, b, err) && has(err, "config set"));
    CCHECK(!config_set_body({"video"}, b, err) && has(err, "<pfad>=<wert>"));
    CCHECK(!config_set_body({"=1"}, b, err));
    CCHECK(!config_set_body({"a=1", "a.b=2"}, b, err) && has(err, "a ist bereits ein Wert"));
    CCHECK(!config_set_body({"a.b=2", "a=1"}, b, err) && has(err, "a ist bereits ein Objekt"));
}

void test_pretty() {
    Json j; std::string err;
    CCHECK(Json::parse("{\"a\":1,\"b\":{\"c\":\"x\",\"d\":[1,2],\"e\":[]},\"f\":[{\"g\":true,\"h\":null},{\"g\":false}],\"i\":2.5}", j, err));
    const std::string want =
        "a: 1\n"
        "b:\n"
        "  c: x\n"
        "  d: 1, 2\n"
        "  e: []\n"
        "f:\n"
        "  - g: true\n"
        "    h: null\n"
        "  - g: false\n"
        "i: 2.5\n";
    CCHECK(pretty(j) == want);
    CCHECK(pretty(Json::object()) == "{}\n");
    CCHECK(pretty(Json::string("s")) == "s\n");
}

void test_parse_http_reply() {
    HttpReply r; std::string err;
    CCHECK(parse_http_reply("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 7\r\nConnection: close\r\n\r\n{\"a\":1}TRAILING", r, err));
    CCHECK(r.status == 200 && r.body == "{\"a\":1}");
    CCHECK(parse_http_reply("HTTP/1.1 409 Conflict\r\n\r\n{\"ok\":false}", r, err) && r.status == 409 && r.body == "{\"ok\":false}");
    CCHECK(parse_http_reply("HTTP/1.0 404 Not Found\ncontent-length: 3\n\nabcdef", r, err) && r.status == 404 && r.body == "abc");
    CCHECK(!parse_http_reply("<html>", r, err) && has(err, "keine HTTP-Antwort"));
    CCHECK(!parse_http_reply("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n", r, err));
    CCHECK(!parse_http_reply("HTTP/1.1 999 X\r\n\r\n", r, err));
}

void test_commands_ipsec() {
    Term t;
    t.http.reply("GET", "/api/v1/ipsec", 200, CFG_JSON);
    t.http.reply("GET", "/api/v1/ipsec/status", 200, STATUS_JSON);
    t.http.reply("PUT", "/api/v1/ipsec/config", 200, "{\"ok\":true,\"pskSet\":true,\"eapPasswordSet\":false,\"caPemSet\":false}");
    t.http.reply("POST", "/api/v1/ipsec/connect", 200, "{\"ok\":true}");
    t.http.reply("POST", "/api/v1/ipsec/disconnect", 200, "{\"ok\":true}");

    // Kurzansicht
    CCHECK(t.run({"ipsec"}) == 0);
    CCHECK(has(t.out, "state=childEstablished") && has(t.out, "runtime=dataPlaneUp") && has(t.out, "daemon=laeuft"));
    // peerIpv4/underlayIpv4 gibt es im echten /api/v1/ipsec/status nicht --
    // die Kurzansicht zeigt nur Felder, die die API wirklich liefert.
    CCHECK(has(t.out, "vpn.example.org:500") && !has(t.out, "peer="));
    CCHECK(has(t.out, "cellular -> cellular (usb0)"));
    CCHECK(has(t.out, "local=cam.test (fqdn)") && has(t.out, "remote=vpn.test (fqdn)"));
    CCHECK(has(t.out, "local=10.77.0.2/32") && has(t.out, "remote=10.66.0.0/24") && has(t.out, "ausgehandelt=10.66.0.0/24"));
    CCHECK(has(t.out, "tunnel=10.77.0.2") && has(t.out, "pfs=nein") && has(t.out, "autoConnect=ja"));
    CCHECK(has(t.out, "algos    : ike=aes256cbc/sha256/dh14  esp=aes256cbc/sha256"));
    CCHECK(has(t.out, "ausgehandelt: ike=aes256cbc/sha256/sha256/dh14  esp=aes256cbc/sha256"));
    CCHECK(has(t.out, "10.66.0.0/24 (tsr, ipsec0)"));
    CCHECK(has(t.out, "ike=udp4500") && has(t.out, "natDetected=ja"));
    CCHECK(has(t.out, "psk=gesetzt") && has(t.out, "eapPassword=nicht gesetzt"));
    CCHECK(has(t.out, "tx 12 Pakete/1024 B") && has(t.out, "uptime 120 s"));
    CCHECK(!has(t.out, "FEHLER"));

    // set: ein PUT mit typisiertem Body, Ausgabe nennt die Felder, Secret nur als (gesetzt)
    t.out.clear();
    CCHECK(t.run({"ipsec", "set", "gateway", "gw.example.net", "port", "4500", "psk", "topsecret"}) == 0);
    CCHECK(t.last().method == "PUT" && t.last().path == "/api/v1/ipsec/config");
    Json sent; std::string err;
    CCHECK(Json::parse(t.last().body, sent, err));
    CCHECK(sent.get("gateway")->as_string() == "gw.example.net" && sent.get("port")->as_int() == 4500 && sent.get("psk")->as_string() == "topsecret");
    CCHECK(has(t.out, "gespeichert: gateway=gw.example.net port=4500 psk=(gesetzt)"));
    CCHECK(has(t.out, "ipsec reconnect"));
    CCHECK(!has(t.out, "topsecret") && !has(t.err, "topsecret"));

    // Bedienfehler: kein Request
    const size_t n = t.http.reqs.size();
    t.err.clear();
    CCHECK(t.run({"ipsec", "set", "gateay", "x"}) == 2 && has(t.err, "gateay") && t.http.reqs.size() == n);
    CCHECK(t.run({"ipsec", "set"}) == 2 && t.http.reqs.size() == n);
    CCHECK(t.run({"ipsec", "frobnicate"}) == 2 && has(t.err, "unbekannt: ipsec frobnicate") && t.http.reqs.size() == n);

    // AP10-Felder, Schreibweise egal
    CCHECK(t.run({"ipsec", "set", "local-id-type", "rfc822", "pfs", "ja", "auto-connect", "nein"}) == 0);
    CCHECK(t.last().body == "{\"localIdType\":\"rfc822\",\"pfs\":true,\"autoConnect\":false}");
    t.err.clear();
    CCHECK(t.run({"ipsec", "set", "remoteIdType", "email"}) == 2 && has(t.err, "fqdn|rfc822|ipv4|keyid"));
    CCHECK(t.run({"ipsec", "set", "requestCp", "true"}) == 2);   // abgeleitet, kein Feld
    t.out.clear();
    CCHECK(t.run({"ipsec", "fields"}) == 0 && has(t.out, "localIdType") && has(t.out, "pfs") && has(t.out, "autoConnect") && has(t.out, "Config Payload"));
    CCHECK(has(t.out, "ipsec algos") && has(t.out, "dpdRetries") && has(t.out, "nattKeepaliveS") && has(t.out, "childLifetimeMb") && has(t.out, "mtu"));

    // AP11: Listen gegen den Katalog -- lokal, vor dem Request
    CCHECK(t.run({"ipsec", "set", "ike-dh", "dh14, dh19", "ikeHash", "sha1,sha256", "mtu", "1300", "dpd", "nein"}) == 0);
    CCHECK(t.last().body == "{\"ikeDh\":[\"dh14\",\"dh19\"],\"ikeHash\":[\"sha1\",\"sha256\"],\"mtu\":1300,\"dpd\":false}");
    const size_t n1 = t.http.reqs.size();
    t.err.clear();
    CCHECK(t.run({"ipsec", "set", "ikeEnc", "chacha20"}) == 2 && has(t.err, "chacha20") && has(t.err, "nicht implementiert") && t.http.reqs.size() == n1);
    t.err.clear();
    CCHECK(t.run({"ipsec", "set", "espHash", "sha3"}) == 2 && has(t.err, "unbekannt") && has(t.err, "sha3") && t.http.reqs.size() == n1);
    CCHECK(t.run({"ipsec", "set", "mtu", "100"}) == 2 && t.run({"ipsec", "set", "dpdRetries", "21"}) == 2);
    // ipsec algos: Katalog aus der API, mit Markierungen
    t.out.clear();
    CCHECK(t.run({"ipsec", "algos"}) == 0);
    CCHECK(has(t.out, "[-] dh2        DH2 - MODP-1024") && has(t.out, "[x] dh14       DH14 - MODP-2048") && has(t.out, "[ ] dh19       DH19 - ECP-256"));
    CCHECK(has(t.out, "[x] sha256     SHA-256                          *") && has(t.out, "ikeHash -- IKE-SA-Hash"));
    // rekey
    t.http.reply("POST", "/api/v1/ipsec/rekey", 200, "{\"ok\":true}");
    t.http.reply("POST", "/api/v1/ipsec/rekey-ike", 200, "{\"ok\":true}");
    t.out.clear();
    CCHECK(t.run({"ipsec", "rekey"}) == 0 && t.last().path == "/api/v1/ipsec/rekey" && has(t.out, "Child-Rekey angefordert"));
    CCHECK(t.run({"ipsec", "rekey", "ike"}) == 0 && t.last().path == "/api/v1/ipsec/rekey-ike");
    CCHECK(t.run({"ipsec", "ikerekey"}) == 0 && t.last().path == "/api/v1/ipsec/rekey-ike");
    t.http.reply("POST", "/api/v1/ipsec/rekey", 409, "{\"ok\":false,\"error\":{\"code\":\"conflict\",\"message\":\"kein Tunnel (Daemon laeuft nicht)\"}}");
    t.err.clear();
    CCHECK(t.run({"ipsec", "rekey"}) == 1 && has(t.err, "kein Tunnel"));
    // Diagnose: die Konsole baut geprueft Kommandozeilen, die Shell fuehrt aus
    t.shell_out = "64 bytes from 192.168.110.11: seq=0 ttl=63 time=45.1 ms\n"; t.out.clear();
    CCHECK(t.run({"ipsec", "ping", "192.168.110.11"}) == 0 && t.shell_cmds.back() == "ping -c 3 -W 2 192.168.110.11 2>&1" && has(t.out, "64 bytes from"));
    CCHECK(t.run({"ipsec", "ping", "fritz.box; rm -rf /"}) == 2 && t.shell_cmds.size() == 1);
    CCHECK(t.run({"ipsec", "ping", "192.168.110.11", "x"}) == 2);
    CCHECK(t.run({"ipsec", "fetch", "192.168.110.1:8080"}) == 0 && has(t.shell_cmds.back(), "http://192.168.110.1:8080/") && has(t.shell_cmds.back(), "curl -s -m 5"));
    CCHECK(t.run({"ipsec", "fetch", "192.168.110.1"}) == 0 && has(t.shell_cmds.back(), "http://192.168.110.1:80/"));
    CCHECK(t.run({"ipsec", "fetch", "192.168.110.1:0"}) == 2 && t.run({"ipsec", "fetch", "example.org"}) == 2);
    CCHECK(t.run({"ipsec", "log"}) == 0 && has(t.shell_cmds.back(), "logread") && has(t.shell_cmds.back(), "grep -i weirdike | tail -n 40"));
    CCHECK(t.run({"ipsec", "log", "5"}) == 0 && has(t.shell_cmds.back(), "tail -n 5"));
    CCHECK(t.run({"ipsec", "log", "0"}) == 2 && t.run({"ipsec", "log", "x"}) == 2);
    t.shell_ok = false; t.err.clear();
    CCHECK(t.run({"ipsec", "ping", "10.0.0.1"}) == 1 && has(t.err, "ping: fehlgeschlagen"));
    t.shell_ok = true;
    { Term noshell; noshell.c.shell = nullptr; CCHECK(noshell.run({"ipsec", "ping", "10.0.0.1"}) == 2 && has(noshell.err, "Kamera-Shell")); }

    // enable/disable
    CCHECK(t.run({"ipsec", "disable"}) == 0 && t.last().body == "{\"enabled\":false}");
    CCHECK(t.run({"ipsec", "enable"}) == 0 && t.last().body == "{\"enabled\":true}");

    // psk: verdeckt gefragt, nie ausgegeben
    t.out.clear(); t.err.clear();
    t.secrets = {"hidden-psk"};
    CCHECK(t.run({"ipsec", "psk"}) == 0);
    CCHECK(t.prompts.size() == 1 && t.prompts[0] == "psk: ");
    CCHECK(t.last().body == "{\"psk\":\"hidden-psk\"}");
    CCHECK(has(t.out, "psk gespeichert") && !has(t.out, "hidden-psk") && !has(t.err, "hidden-psk"));
    CCHECK(t.run({"ipsec", "psk", "arg-psk"}) == 0 && t.last().body == "{\"psk\":\"arg-psk\"}");
    t.secrets = {""}; t.next_secret = 0;
    const size_t n2 = t.http.reqs.size();
    CCHECK(t.run({"ipsec", "psk"}) == 2 && t.http.reqs.size() == n2);   // leer -> nichts gesendet
    CCHECK(t.run({"ipsec", "eap-password", "eap-pw"}) == 0 && t.last().body == "{\"eapPassword\":\"eap-pw\"}");

    // get: Schreibweise egal; Secrets nur als Anwesenheit
    t.out.clear();
    CCHECK(t.run({"ipsec", "get", "remote-subnet"}) == 0 && has(t.out, "remoteSubnet: 10.66.0.0/24\n"));
    t.out.clear();
    CCHECK(t.run({"ipsec", "get", "ikeEnc"}) == 0 && has(t.out, "ikeEnc: aes256cbc\n"));
    t.out.clear();
    CCHECK(t.run({"ipsec", "get", "psk"}) == 0 && has(t.out, "psk: write-only (gesetzt: ja)"));
    t.out.clear();
    CCHECK(t.run({"ipsec", "get", "extraPem"}) == 0 && has(t.out, "extraPem: write-only (gesetzt: nein)"));
    CCHECK(t.run({"ipsec", "get", "nope"}) == 2);

    // connect: POST, danach der Stand
    t.out.clear();
    CCHECK(t.run({"ipsec", "connect"}) == 0);
    CCHECK(t.http.reqs[t.http.reqs.size() - 2].method == "POST" && t.http.reqs[t.http.reqs.size() - 2].path == "/api/v1/ipsec/connect");
    CCHECK(has(t.out, "Verbindung wird aufgebaut. Stand: dataPlaneUp (childEstablished)"));
    t.out.clear();
    CCHECK(t.run({"ipsec", "disconnect"}) == 0 && has(t.out, "getrennt"));
    t.out.clear();
    CCHECK(t.run({"ipsec", "reconnect"}) == 0 && has(t.out, "getrennt, baue neu auf") && has(t.out, "Verbindung wird aufgebaut"));

    // API-Fehler: Status + Code + Meldung, Rueckgabe 1
    t.http.reply("POST", "/api/v1/ipsec/connect", 409, "{\"ok\":false,\"error\":{\"code\":\"conflict\",\"path\":\"/api/v1/ipsec/connect\",\"message\":\"kein Gateway konfiguriert\"}}");
    t.err.clear();
    CCHECK(t.run({"ipsec", "connect"}) == 1 && has(t.err, "Fehler: HTTP 409 conflict: kein Gateway konfiguriert"));

    // Nicht verdrahtet (404 mit JSON) vs. fremder Server (404 ohne JSON)
    t.http.reply("GET", "/api/v1/ipsec/status", 404, "{\"ok\":false,\"error\":{\"code\":\"not_found\",\"message\":\"ipsec ist auf dieser Plattform nicht verdrahtet\"}}");
    t.err.clear();
    CCHECK(t.run({"ipsec", "status"}) == 1 && has(t.err, "nicht verdrahtet"));
    t.http.reply("GET", "/api/v1/ipsec/status", 404, "<html>majestic</html>");
    t.err.clear();
    CCHECK(t.run({"ipsec", "status"}) == 1 && has(t.err, "ohne JSON-Antwort") && has(t.err, "streamerctl status"));

    // fehlgeschlagener Tunnel in der Kurzansicht
    t.http.reply("GET", "/api/v1/ipsec/status", 200, "{\"daemonRunning\":true,\"state\":\"failed\",\"runtimeState\":\"failed\",\"manualStop\":false,\"reconnectAttempt\":3,\"rawState\":\"FAILED\",\"failure\":{\"code\":\"authenticationFailed\",\"lastNotify\":24},\"natT\":true,\"natDetected\":false,\"requestedUnderlay\":\"cellular\",\"routes\":[],\"childGeneration\":0,\"ikeGeneration\":0,\"uptimeS\":0,\"txPackets\":0,\"txBytes\":0,\"rxPackets\":0,\"rxBytes\":0}");
    t.out.clear();
    CCHECK(t.run({"ipsec"}) == 0);
    CCHECK(has(t.out, "FEHLER   : authenticationFailed (notify 24)") && has(t.out, "routes   : (keine installiert)") && has(t.out, "reconnect: Versuch 3 geplant"));

    // Transport weg: Rueckgabe 1, Hinweis auf streamerctl
    t.http.down = true; t.err.clear();
    CCHECK(t.run({"ipsec"}) == 1 && has(t.err, "machinod nicht erreichbar") && has(t.err, "streamerctl status"));
    t.http.down = false;

    // fields + help sind reine Ausgaben
    t.out.clear();
    CCHECK(t.run({"ipsec", "fields"}) == 0 && has(t.out, "remoteSubnet") && has(t.out, "anchor-pem|host-store|host-store-plus-pem|none") && has(t.out, "write-only"));
    t.out.clear();
    CCHECK(t.run({"help"}) == 0 && has(t.out, "ipsec setup") && has(t.out, "--port N"));
    t.err.clear();
    CCHECK(t.run({"frob"}) == 2 && has(t.err, "unbekannter Befehl: frob"));
    CCHECK(t.run({}) == 2);
}

void test_commands_json_mode() {
    Term t;
    t.c.raw_json = true;
    t.http.reply("GET", "/api/v1/ipsec", 200, CFG_JSON);
    t.http.reply("GET", "/api/v1/ipsec/status", 200, STATUS_JSON);
    t.http.reply("GET", "/api/v1/state", 200, "{\"lifecycle\":\"cold_idle\"}");
    CCHECK(t.run({"status"}) == 0 && t.out == "{\"lifecycle\":\"cold_idle\"}\n");
    t.out.clear();
    CCHECK(t.run({"ipsec", "get", "ikeEnc"}) == 0 && t.out == "[\"aes256cbc\"]\n");
    t.out.clear();
    CCHECK(t.run({"ipsec"}) == 0);
    Json j; std::string err;
    CCHECK(Json::parse(t.out, j, err) && j.get("config") && j.get("status") && j.get("status")->get("state")->as_string() == "childEstablished");
}

void test_commands_config_and_api() {
    Term t;
    t.http.reply("GET", "/api/v1/config", 200, "{\"revision\":3,\"video\":{\"0\":{\"fps\":20,\"bitrate_kbps\":3000}}}");
    t.http.reply("PATCH", "/api/v1/config", 200, "{\"ok\":true,\"changes\":[{\"path\":\"video.0.fps\",\"status\":\"stored\"}]}");
    t.http.reply("GET", "/api/v1/state", 200, "{\"lifecycle\":\"cold_idle\",\"consumers\":{\"total\":0}}");
    t.http.reply("GET", "/metrics", 200, "machino_up 1\n");
    t.http.reply("POST", "/api/v1/network/change", 200, "{\"ok\":true,\"id\":\"7\"}");

    CCHECK(t.run({"status"}) == 0 && has(t.out, "lifecycle: cold_idle\n") && has(t.out, "consumers:\n  total: 0\n"));
    t.out.clear();
    CCHECK(t.run({"config"}) == 0 && has(t.out, "revision: 3\n") && has(t.out, "    fps: 20\n"));
    t.out.clear();
    CCHECK(t.run({"config", "get", "video.0.fps"}) == 0 && t.out == "video.0.fps: 20\n");
    t.out.clear();
    CCHECK(t.run({"config", "get", "video.0"}) == 0 && has(t.out, "fps: 20\n") && has(t.out, "bitrate_kbps: 3000\n"));
    t.err.clear();
    CCHECK(t.run({"config", "get", "video.9"}) == 1 && has(t.err, "video.9: nicht vorhanden"));
    t.out.clear();
    CCHECK(t.run({"config", "set", "video.0.fps=15", "video.0.bitrate_kbps=1500"}) == 0);
    CCHECK(t.last().method == "PATCH" && t.last().body == "{\"video\":{\"0\":{\"fps\":15,\"bitrate_kbps\":1500}}}");
    CCHECK(has(t.out, "changes:\n  - path: video.0.fps\n    status: stored\n"));
    CCHECK(t.run({"config", "set"}) == 2 && t.run({"config", "frob"}) == 2);

    // api: Durchgriff, Nicht-JSON wird roh gezeigt, Body geprueft
    t.out.clear();
    CCHECK(t.run({"api", "get", "/metrics"}) == 0 && t.out == "machino_up 1\n");
    t.out.clear();
    CCHECK(t.run({"api", "post", "/api/v1/network/change", "{\"uplink\":\"cellular\"}"}) == 0);
    CCHECK(t.last().method == "POST" && t.last().body == "{\"uplink\":\"cellular\"}" && has(t.out, "id: 7\n"));
    const size_t n = t.http.reqs.size();
    t.err.clear();
    CCHECK(t.run({"api", "post", "/x", "nope"}) == 2 && has(t.err, "kein JSON") && t.http.reqs.size() == n);
    CCHECK(t.run({"api", "get", "api/v1/state"}) == 2 && t.http.reqs.size() == n);
    CCHECK(t.run({"api", "get", "/api/v1/events"}) == 2 && t.http.reqs.size() == n);
    CCHECK(t.run({"api", "head", "/x"}) == 2 && t.run({"api", "get"}) == 2);
    // Body aus stdin ('-')
    t.lines = {"{\"a\":", "1}"}; t.next = 0;
    CCHECK(t.run({"api", "put", "/api/v1/network/change", "-"}) == 1);   // kein Fake fuer PUT -> 404, aber gesendet:
    CCHECK(t.last().method == "PUT" && t.last().body == "{\"a\":\n1}\n");
}

void test_repl() {
    Term t;
    t.http.reply("GET", "/api/v1/ipsec", 200, CFG_JSON);
    t.lines = {"", "   ", "ipsec get gateway", "bogus", "EXIT", "ipsec get port"};
    t.c.interactive = false;
    CCHECK(run_repl(t.c) == 0);
    CCHECK(t.out == "gateway: vpn.example.org\n");            // kein Prompt, kein Banner bei Pipe
    CCHECK(has(t.err, "unbekannter Befehl: bogus"));
    CCHECK(t.http.reqs.size() == 1);                           // nach exit nichts mehr
    // interaktiv: Banner + Prompt, EOF beendet sauber
    Term u;
    u.lines = {"help"};
    CCHECK(run_repl(u.c) == 0);
    CCHECK(has(u.out, "machinoctl -- Machino-Konsole") && has(u.out, "machino> ") && has(u.out, "Befehle:"));
    CCHECK(u.out.size() >= 1 && u.out.back() == '\n');
}

void test_setup_wizard() {
    Term t;
    t.http.reply("GET", "/api/v1/ipsec", 200, "{\"enabled\":false,\"gateway\":\"\",\"port\":500,\"underlay\":\"auto\",\"localId\":\"\",\"remoteId\":\"\",\"localSubnet\":\"\",\"remoteSubnet\":\"\",\"natT\":true,\"pskSet\":false,\"auth\":\"psk\",\"eapUser\":\"\",\"trustMode\":\"host-store\",\"eapPasswordSet\":false,\"caPemSet\":false,\"extraPemSet\":false}");
    t.http.reply("PUT", "/api/v1/ipsec/config", 200, "{\"ok\":true,\"pskSet\":true}");
    t.http.reply("POST", "/api/v1/ipsec/connect", 200, "{\"ok\":true}");
    t.http.reply("GET", "/api/v1/ipsec/status", 200, "{\"daemonRunning\":true,\"state\":\"connecting\",\"runtimeState\":\"ikeConnecting\"}");
    t.lines = {"vpn.example.org", "70000", "",            // Port: erst ungueltig, dann Vorgabe 500
               "lte", "cellular",                          // Underlay: erst ungueltig
               "cam.example.org", "",                      // lokale ID + Typ (Vorgabe fqdn)
               "vpn.example.org", "mail", "rfc822",        // Remote-ID + Typ (erst ungueltig)
               "10.77.0.2/32", "192.168.178.0/24",
               "",                                         // auth: Vorgabe psk
               "nein",                                     // NAT-T
               "ja",                                       // PFS
               "",                                         // enabled: Vorgabe ja
               "nein",                                     // autoConnect
               "ja"};                                      // verbinden
    t.secrets = {"wizard-psk"};
    CCHECK(t.run({"ipsec", "setup"}) == 0);
    // Der PUT traegt alles, was gefragt wurde, mit den richtigen Typen.
    const FakeHttp::Req* put = nullptr;
    for (const auto& r : t.http.reqs) if (r.method == "PUT") put = &r;
    CCHECK(put != nullptr);
    Json b; std::string err;
    CCHECK(put && Json::parse(put->body, b, err));
    if (put) {
        CCHECK(b.get("gateway")->as_string() == "vpn.example.org" && b.get("port")->as_int() == 500);
        CCHECK(b.get("underlay")->as_string() == "cellular" && b.get("localId")->as_string() == "cam.example.org");
        CCHECK(b.get("remoteSubnet")->as_string() == "192.168.178.0/24" && b.get("auth")->as_string() == "psk");
        CCHECK(b.get("psk")->as_string() == "wizard-psk" && !b.get("natT")->as_bool() && b.get("enabled")->as_bool());
        CCHECK(!b.has("eapUser") && !b.has("eapPassword") && !b.has("trustMode"));
        CCHECK(b.get("localIdType")->as_string() == "fqdn" && b.get("remoteIdType")->as_string() == "rfc822");
        CCHECK(b.get("pfs")->as_bool() && !b.get("autoConnect")->as_bool());
        CCHECK(!b.has("requestCp"));                     // abgeleitet: leere localSubnet = CP
    }
    CCHECK(t.last().path == "/api/v1/ipsec/status" && t.http.reqs[t.http.reqs.size() - 2].path == "/api/v1/ipsec/connect");
    CCHECK(has(t.out, "Ganzzahl 1..65535") && has(t.out, "auto, ethernet, wifi oder cellular") && has(t.out, "fqdn, rfc822, ipv4 oder keyid"));
    CCHECK(has(t.out, "leer = automatisch vom Gateway"));
    CCHECK(has(t.out, "gespeichert:") && has(t.out, "psk=(gesetzt)") && !has(t.out, "wizard-psk") && !has(t.err, "wizard-psk"));
    CCHECK(has(t.out, "Stand: ikeConnecting (connecting)"));
    CCHECK(t.prompts.size() == 1 && has(t.prompts[0], "PSK (leer = keiner"));

    // EOF mitten im Dialog: nichts gespeichert
    Term u;
    u.http.reply("GET", "/api/v1/ipsec", 200, CFG_JSON);
    u.lines = {"gw"};
    CCHECK(u.run({"ipsec", "setup"}) == 2 && has(u.err, "abgebrochen"));
    for (const auto& r : u.http.reqs) CCHECK(r.method == "GET");

    // EAP-Zweig: Benutzer, Passwort, Trust-Modus
    Term e;
    e.http.reply("GET", "/api/v1/ipsec", 200, CFG_JSON);
    e.http.reply("PUT", "/api/v1/ipsec/config", 200, "{\"ok\":true}");
    // gateway, port, underlay, localId, localIdType, remoteId, remoteIdType, localSubnet, remoteSubnet,
    // auth, eapUser, [secret], trust, natT, pfs, enabled, autoConnect
    e.lines = {"", "", "", "", "", "", "", "-", "", "eap-mschapv2", "alice", "none", "", "", "nein", ""};
    e.secrets = {"eap-secret"};
    CCHECK(e.run({"ipsec", "setup"}) == 0);
    CCHECK(Json::parse(e.last().body, b, err));
    CCHECK(b.get("auth")->as_string() == "eap-mschapv2" && b.get("eapUser")->as_string() == "alice");
    CCHECK(b.get("eapPassword")->as_string() == "eap-secret" && b.get("trustMode")->as_string() == "none" && !b.has("psk"));
    CCHECK(!b.get("enabled")->as_bool());
    CCHECK(b.get("localSubnet")->as_string().empty());   // '-' leert: Tunnel-Adresse vom Gateway
    CCHECK(b.get("autoConnect")->as_bool() && !b.get("pfs")->as_bool());   // Vorgaben uebernommen
    CCHECK(!has(e.out, "eap-secret") && !has(e.err, "eap-secret"));
    for (const auto& r : e.http.reqs) CCHECK(r.method != "POST");   // enabled=nein -> keine Verbindungsfrage
}

// ---- Vertragsbeweis: die Konsole gegen die ECHTE ApiService ----------------

hw::ResolvedHardware make_hw() {
    hw::Registry r;
    r.add_platform({"fake", "f", "m"});
    hw::SensorDescriptor s; s.model = "s"; s.interface = hw::SensorInterface::MipiCsi; s.native_width = 1920; s.native_height = 1080;
    s.modes = { {1920, 1080, 20}, {1920, 1080, 15}, {1920, 1080, 10} };
    r.add_sensor(s);
    hw::BoardProfile b; b.board_id = "board-x"; b.platform = "fake-m"; b.sensor = "s";
    b.wiring.i2c_bus = 0; b.wiring.i2c_addr = 0x10; b.wiring.mclk = 0; b.default_mode = hw::SensorMode{1920, 1080, 20};
    b.presets.balanced_fps = 15; b.presets.battery_fps = 10; b.presets.battery_bitrate = 1200; b.hardware_verified = true;
    r.add_board(b);
    hw::UserHardwareConfig u; u.board_id = "board-x";
    hw::ResolvedHardware hw; std::string err;
    CCHECK(hw::resolve_hardware(u, r, {}, hw, err));
    return hw;
}

struct CtlFakeIpsecBackend : ipsec::IIpsecBackend {
    bool running = false;
    std::string status_text;
    bool daemon_running() override { return running; }
    bool start_daemon(std::string&) override { running = true; return true; }
    bool stop_daemon(std::string&) override { running = false; return true; }
    bool ctl_status(std::string& out) override { out = status_text; return running; }
    bool resolve4(const std::string&, std::string& out) override { out = "192.0.2.7"; return true; }
    bool add_peer_route(const std::string&, const std::string&, const std::string&, std::string&) override { return true; }
    bool del_peer_route(const std::string&, const std::string&, const std::string&, std::string&) override { return true; }
};

// Der Transport, den die Konsole in Wirklichkeit ueber 127.0.0.1 erreicht --
// hier direkt an die Routen von ApiService gehaengt (dieselbe Zuordnung wie
// in http_server.cpp).
struct ApiHttp : IHttpClient {
    api::ApiService& api;
    explicit ApiHttp(api::ApiService& a) : api(a) {}
    bool request(const std::string& m, const std::string& p, const std::string& body, HttpReply& out, std::string&) override {
        api::Response r;
        if      (p == "/api/v1/ipsec"            && m == "GET")  r = api.ipsec_get();
        else if (p == "/api/v1/ipsec/config"     && m == "PUT")  r = api.ipsec_put_config(body);
        else if (p == "/api/v1/ipsec/connect"    && m == "POST") r = api.ipsec_connect();
        else if (p == "/api/v1/ipsec/disconnect" && m == "POST") r = api.ipsec_disconnect();
        else if (p == "/api/v1/ipsec/status"     && m == "GET")  r = api.ipsec_status();
        else if (p == "/api/v1/ipsec/rekey"      && m == "POST") r = api.ipsec_rekey(false);
        else if (p == "/api/v1/state"            && m == "GET")  r = api.state();
        else if (p == "/api/v1/config"           && m == "GET")  r = api.config();
        else if (p == "/api/v1/config"           && m == "PATCH") r = api.patch_config(body, "");
        else r = api::ApiService::fail(404, "not_found", p, "keine solche Route");
        out.status = r.status;
        out.body = r.body.dump();
        return true;
    }
};

void test_against_real_api() {
    const char* TMP = "tests/tmp_ctl_machino.conf";
    const char* MC = "test_ctl_ipsec_m.conf";
    const char* DC = "test_ctl_ipsec_d.conf";
    remove(MC); remove(DC);
    test::CallLog log; test::FakePowerControl power; test::FakePlatform platform{log, &power};
    test::FakeTimer timer; StreamHub hub; test::FakeStats stats; EventBus bus;
    ConfigStore store{TMP};
    { FILE* f = fopen(TMP, "w"); if (f) { fputs("board = board-x\nvideo.bitrate = 3000\n", f); fclose(f); } }
    std::string lerr; store.load(lerr);
    hw::ResolvedHardware hw = make_hw(); AppConfig cfg;
    EffectiveStream stream = effective_stream(cfg.video, hw);
    lifecycle::LifecycleConfig lc; lc.idle_grace_ms = 1000; lc.poll_timeout_ms = 10;
    lifecycle::PipelineManager mgr(platform, stream, lc, timer, hub);
    power::PerformanceService perf(mgr, platform, stats, hw, cfg.video);
    media::TuningService tuning(mgr, platform, hub, stream, cfg.image, cfg.latency);
    detection::DetectionService detection(mgr, platform, bus, cfg.ai);
    api::ApiService api(perf, tuning, mgr, store, bus, hw, cfg, &detection);
    CtlFakeIpsecBackend be;
    ipsec::IpsecService svc(be, MC, DC);
    api.set_ipsec_service(&svc);

    ApiHttp http(api);
    std::string out, err;
    Console c{http, [&](const std::string& s) { out += s; }, [&](const std::string& s) { err += s; },
              nullptr, [](const std::string&) { return std::string("real-psk-geheim"); }, nullptr, false, true};

    // Jedes Feld der Konsole nimmt die API an -- KEIN unknown_field.
    CCHECK(run_command(c, {"ipsec", "set",
        "enabled", "false", "gateway", "vpn.example.org", "port", "4500", "underlay", "auto",
        "localId", "cam.test", "remoteId", "vpn.test", "localSubnet", "10.77.0.2/32", "remoteSubnet", "10.66.0.0/24",
        "natT", "true", "dpdIntervalS", "20", "ikeLifetimeS", "0", "childLifetimeS", "0",
        "ikeEnc", "aes256cbc", "ikeHash", "sha256", "ikeDh", "dh14", "espEnc", "aes256cbc", "espHash", "sha256",
        "auth", "psk", "eapUser", "", "trustMode", "host-store",
        "localIdType", "rfc822", "remoteIdType", "fqdn", "pfs", "true", "autoConnect", "false"}) == 0);
    CCHECK(err.empty());
    CCHECK(has(out, "gespeichert:") && has(out, "gateway=vpn.example.org") && has(out, "localIdType=rfc822"));
    out.clear();
    CCHECK(run_command(c, {"ipsec", "get", "local-id-type"}) == 0 && out == "localIdType: rfc822\n");
    // Typ passt nicht zum Wert: die API lehnt MIT NAMEN ab, die Konsole reicht es durch.
    out.clear(); err.clear();
    CCHECK(run_command(c, {"ipsec", "set", "remoteIdType", "ipv4"}) == 1 && has(err, "remoteId"));

    // Die API lehnt einen fremden Algorithmus MIT NAMEN ab -- die Konsole reicht das durch.
    // AP11: die Konsole lehnt Nicht-Implementiertes selbst ab (derselbe Katalog);
    // eine breitere, implementierte Liste nimmt die API an, 'ipsec algos' zeigt sie.
    out.clear(); err.clear();
    CCHECK(run_command(c, {"ipsec", "set", "ikeEnc", "chacha20"}) == 2);
    CCHECK(has(err, "chacha20") && has(err, "nicht implementiert"));
    out.clear(); err.clear();
    CCHECK(run_command(c, {"ipsec", "set", "ikeDh", "dh14,dh19,dh31", "ikeHash", "sha1,sha256,sha512", "espHash", "sha1,sha256"}) == 0);
    out.clear();
    CCHECK(run_command(c, {"ipsec", "algos"}) == 0);
    CCHECK(has(out, "[x] dh31       DH31 - Curve25519") && has(out, "[-] dh32       DH32 - Curve448") && has(out, "[x] sha512     SHA-512") && has(out, "[ ] sha384     SHA-384"));
    CCHECK(has(out, "[-] chacha20   ChaCha20-Poly1305") && has(out, "[-] null       NULL"));

    // enable ohne PSK: die API verlangt ihn -- und die Meldung nennt keinen Wert.
    out.clear(); err.clear();
    CCHECK(run_command(c, {"ipsec", "enable"}) == 1 && has(err, "psk"));

    // PSK verdeckt setzen, dann enable + connect; das Secret taucht nirgends auf.
    out.clear(); err.clear();
    CCHECK(run_command(c, {"ipsec", "psk"}) == 0);
    CCHECK(run_command(c, {"ipsec", "enable"}) == 0);
    CCHECK(run_command(c, {"ipsec", "get", "psk"}) == 0 && has(out, "psk: write-only (gesetzt: ja)"));
    CCHECK(run_command(c, {"ipsec", "connect"}) == 0 && be.running);
    be.status_text = "state=CHILD_SA_ESTABLISHED\ninterface=ipsec0\nlast_notify=0\n";
    CCHECK(run_command(c, {"ipsec"}) == 0);
    CCHECK(has(out, "algos    : ike=aes256cbc/sha1,sha256,sha512/dh14,dh19,dh31  esp=aes256cbc/sha1,sha256"));
    err.clear();
    CCHECK(run_command(c, {"ipsec", "rekey"}) == 1 && has(err, "abgelehnt"));   // Fake-Backend kann kein rekey -> 409, benannt
    CCHECK(has(out, "state=childEstablished") && has(out, "vpn.example.org:4500") && has(out, "psk=gesetzt"));
    CCHECK(has(out, "cam.test (rfc822)") && has(out, "pfs=ja") && has(out, "autoConnect=nein"));
    CCHECK(run_command(c, {"ipsec", "config"}) == 0 && has(out, "pskSet: true"));
    CCHECK(!has(out, "real-psk-geheim") && !has(err, "real-psk-geheim"));
    CCHECK(run_command(c, {"ipsec", "disconnect"}) == 0 && !be.running);

    // Der Rest der Konsole gegen die echte API: status, config get/set.
    out.clear();
    CCHECK(run_command(c, {"status"}) == 0 && has(out, "lifecycle: cold_idle\n"));
    out.clear();
    CCHECK(run_command(c, {"config", "get", "video.0.bitrate_kbps"}) == 0 && out == "video.0.bitrate_kbps: 3000\n");
    out.clear();
    CCHECK(run_command(c, {"config", "set", "video.0.bitrate_kbps=1500"}) == 0 && has(out, "path: video.0.bitrate_kbps") && store.get("video.bitrate") == "1500");

    remove(MC); remove(DC); remove(TMP);
}

} // namespace

void run_ctl_tests() {
    test_split_line();
    test_field_names();
    test_ipsec_set_body();
    test_config_set_body();
    test_pretty();
    test_parse_http_reply();
    test_commands_ipsec();
    test_commands_json_mode();
    test_commands_config_and_api();
    test_repl();
    test_setup_wizard();
    test_against_real_api();
}
