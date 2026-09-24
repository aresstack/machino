// AP-M7: PPP als Alternativpfad.
//
// Die Faelle hier sind die, an denen eine Einwahl in der Praxis scheitert, und
// vor allem die, die sich von aussen gleich anfuehlen und verschieden sind:
// falsches Passwort, kein CONNECT, haengende Aushandlung, Gegenstelle legt
// auf. Wer sie nicht auseinanderhaelt, schickt jemanden mit "keine
// Verbindung" auf die Suche.
#include "core/cellular/ppp_link.hpp"
#include "app/api/net_views.hpp"
#include "core/net/cellular_uplink.hpp"
#include "scripted_at_transport.hpp"
#include <cstdio>
#include <string>
#include <vector>

using namespace machino;
using namespace machino::cellular;
using machino::test::ScriptedAtTransport;

extern int g_fail_ext, g_pass_ext;
#define TCHECK(cond) do { if (cond) { ++g_pass_ext; } else { ++g_fail_ext; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

namespace {

// Ein pppd, das der Test steuert -- und das mitschreibt, was von ihm verlangt
// wurde. Der Punkt ist nicht nur, WAS es meldet, sondern auch, dass es
// ueberhaupt gestartet wurde. Oder eben nicht.
class FakePppBackend : public IPppBackend {
public:
    int starts = 0, stops = 0;
    PppRequest last;
    PppStatus  st;
    bool readable = true;        // false = "konnte nicht nachsehen"
    bool start_fails = false;

    Result start(const PppRequest& req) override
    {
        ++starts;
        last = req;
        if (start_fails) return Result::error();
        st = PppStatus{};
        st.running = true;
        return Result::ok();
    }
    Result stop() override
    {
        ++stops;
        st = PppStatus{};
        return Result::ok();
    }
    bool status(PppStatus& out) const override
    {
        if (!readable) return false;
        out = st;
        return true;
    }

    void negotiated(const char* ifname, const char* ip, const char* gw)
    {
        st.running = true;
        st.ifname = ifname;
        st.address.ipv4 = ip;
        st.address.gateway = gw;
    }
    void died(PppExit e)
    {
        st = PppStatus{};
        st.running = false;
        st.exit = e;
    }
};

struct Clock { uint64_t t = 1000; };

CellularStatus ready_status()
{
    CellularStatus s;
    s.present = true;
    s.responsive = true;
    s.sim = SimState::Ready;
    s.registration = RegState::RegisteredHome;
    return s;
}

CellularConfig ppp_config()
{
    CellularConfig c;
    c.enabled = true;
    c.apn = "internet.t-d1.de";
    c.data_link = "ppp";
    return c;
}

struct Rig {
    ScriptedAtTransport at;
    FakePppBackend      be;
    Clock               clock;
    PppLink             link{at, be};

    Rig()
    {
        link.set_clock([this] { return clock.t; });
        link.set_config(ppp_config());
        link.set_modem_port("/dev/ttyUSB4");
        at.reply("AT+CGACT=0,1", "OK\r\n");
        at.reply("AT+CGDCONT=1,\"IP\",\"internet.t-d1.de\"", "OK\r\n");
    }
    DataLinkState run(int n = 6, uint64_t step = 500)
    {
        for (int i = 0; i < n; ++i) {
            const DataLinkState s = link.tick(ready_status()).state;
            if (s == DataLinkState::Up || s == DataLinkState::Failed) return s;
            clock.t += step;
        }
        return link.state().state;
    }
};

// ------------------------------------------------------------- Auswahl ----

void test_ppp_is_not_started_until_someone_asks()
{
    Rig r;
    // Kein connect(): die Maschine tut nichts, und pppd wird nicht gestartet.
    for (int i = 0; i < 5; ++i) r.link.tick(ready_status());
    TCHECK(r.link.state().state == DataLinkState::Disabled);
    TCHECK(r.be.starts == 0);
    TCHECK(r.at.sent().empty());
}

void test_the_link_says_which_kind_it_is()
{
    // Die Oberflaeche zeigt EIN Mobilfunkgeraet. Welcher Datenlink darunter
    // liegt, gehoert trotzdem in den Status -- eine Fehlersuche ohne diese
    // Angabe stochert im Dunkeln.
    Rig r;
    TCHECK(r.link.state().kind == DataLinkKind::Ppp);

    DataLinkKind k;
    TCHECK(data_link_kind_parse("ppp", k) && k == DataLinkKind::Ppp);
    TCHECK(data_link_kind_parse("ecm", k) && k == DataLinkKind::Ecm);
    TCHECK(!data_link_kind_parse("pppoe", k));
    TCHECK(std::string(data_link_kind_name(DataLinkKind::Ppp)) == "ppp");
}

// -------------------------------------------------------- der gute Fall ----

void test_a_successful_dial_comes_up()
{
    Rig r;
    r.link.connect();

    // Erster Tick: PDP setzen und waehlen.
    TCHECK(r.link.tick(ready_status()).state == DataLinkState::Dial);
    TCHECK(r.be.starts == 1);
    TCHECK(r.be.last.tty == "/dev/ttyUSB4");
    TCHECK(r.be.last.apn == "internet.t-d1.de");
    // Die Einwahlnummer aus der Referenz, wenn nichts anderes konfiguriert ist.
    TCHECK(r.be.last.dial == "*99***1#");

    // Der PDP-Kontext wurde VOR dem Waehlen abgeraeumt und neu gesetzt.
    TCHECK(r.at.count_sent("AT+CGACT=0,1") == 1);
    TCHECK(r.at.count_sent("AT+CGDCONT=1,\"IP\",\"internet.t-d1.de\"") == 1);

    // pppd verhandelt: Interface da, Adresse noch nicht.
    r.be.st.ifname = "ppp0";
    r.clock.t += 500;
    TCHECK(r.link.tick(ready_status()).state == DataLinkState::Negotiating);

    r.be.negotiated("ppp0", "10.64.2.7", "10.64.2.8");
    r.clock.t += 500;
    const CellularLinkState& s = r.link.tick(ready_status());
    TCHECK(s.state == DataLinkState::Up);
    TCHECK(s.interface_name == "ppp0");
    TCHECK(s.address.ipv4 == "10.64.2.7");
    TCHECK(s.address.gateway == "10.64.2.8");
    TCHECK(s.kind == DataLinkKind::Ppp);
}

void test_the_interface_name_is_never_assumed()
{
    // ppp0 ist der haeufige Fall und nicht der einzige. Laeuft schon ein
    // anderer Anruf, heisst dieser ppp1 -- und ein hartkodiertes ppp0 zeigte
    // dann auf eine fremde Verbindung.
    Rig r;
    r.link.connect();
    r.link.tick(ready_status());
    r.be.negotiated("ppp3", "10.1.2.3", "10.1.2.4");
    r.clock.t += 500;
    TCHECK(r.link.tick(ready_status()).state == DataLinkState::Up);
    TCHECK(r.link.state().interface_name == "ppp3");
}

// ------------------------------------------------------------- Fehler ------

void test_the_dial_waits_for_the_sim()
{
    // NICHT waehlen, solange die SIM nicht bereit ist. Der Dial liefe ins
    // Leere, und die Meldung waere "kein CONNECT" statt "PIN fehlt".
    Rig r;
    r.link.connect();
    CellularStatus s = ready_status();
    s.sim = SimState::PinRequired;
    s.sim_detail = "SIM PIN erforderlich";

    for (int i = 0; i < 5; ++i) { r.link.tick(s); r.clock.t += 500; }
    TCHECK(r.link.state().state == DataLinkState::WaitSim);
    TCHECK(r.link.state().detail == "SIM PIN erforderlich");
    TCHECK(r.be.starts == 0);
    TCHECK(r.at.count_sent("AT+CGACT=0,1") == 0);
}

void test_the_dial_waits_for_registration()
{
    Rig r;
    r.link.connect();
    CellularStatus s = ready_status();
    s.registration = RegState::Searching;
    for (int i = 0; i < 3; ++i) { r.link.tick(s); r.clock.t += 500; }
    TCHECK(r.link.state().state == DataLinkState::WaitRegistration);
    TCHECK(r.be.starts == 0);
}

void test_no_modem_port_is_a_wait_not_a_failure()
{
    // Nach einer Re-Enumeration heisst der Port anders und ist kurz weg. Das
    // als Fehlschlag zu zaehlen liefe in den Backoff, obwohl nichts kaputt ist.
    Rig r;
    r.link.set_modem_port("");
    r.link.connect();
    for (int i = 0; i < 4; ++i) { r.link.tick(ready_status()); r.clock.t += 500; }
    TCHECK(r.link.state().state == DataLinkState::WaitDevice);
    TCHECK(r.link.state().attempts == 0);
    TCHECK(r.be.starts == 0);

    // Taucht er auf, geht es weiter.
    r.link.set_modem_port("/dev/ttyUSB4");
    TCHECK(r.link.tick(ready_status()).state == DataLinkState::Dial);
}

void test_an_authentication_failure_says_so()
{
    Rig r;
    r.link.connect();
    r.link.tick(ready_status());
    r.be.died(PppExit::AuthFailed);
    r.clock.t += 500;
    TCHECK(r.link.tick(ready_status()).state == DataLinkState::Failed);
    TCHECK(r.link.state().detail.find("Authentifizierung") != std::string::npos);
    // Und das Passwort steht NICHT in der Begruendung.
    TCHECK(r.link.state().detail.find("hunter2") == std::string::npos);
}

void test_a_dial_that_never_connects_says_so()
{
    Rig r;
    r.link.connect();
    r.link.tick(ready_status());
    r.be.died(PppExit::DialFailed);
    r.clock.t += 500;
    TCHECK(r.link.tick(ready_status()).state == DataLinkState::Failed);
    const std::string d = r.link.state().detail;
    TCHECK(d.find("CONNECT") != std::string::npos);
    TCHECK(d.find("/dev/ttyUSB4") != std::string::npos);
}

void test_the_other_failures_are_each_their_own_sentence()
{
    struct Case { PppExit e; const char* needle; };
    const Case cases[] = {
        {PppExit::NegotiationFailed, "Aushandlung"},
        {PppExit::NoDevice,          "Modem-Port"},
        {PppExit::LinkTerminated,    "Gegenstelle"},
    };
    for (const Case& c : cases) {
        Rig r;
        r.link.connect();
        r.link.tick(ready_status());
        r.be.died(c.e);
        r.clock.t += 500;
        TCHECK(r.link.tick(ready_status()).state == DataLinkState::Failed);
        TCHECK(r.link.state().detail.find(c.needle) != std::string::npos);
    }
}

void test_a_hanging_negotiation_is_broken_off()
{
    // LCP/IPCP sind normalerweise in unter fuenf Sekunden durch. Bleibt es
    // stehen, kommt gar kein Ereignis mehr -- laenger zu warten heilt das
    // nicht, und in der Referenz war genau das das "erst beim zweiten Mal"-
    // Symptom.
    Rig r;
    r.link.connect();
    r.link.tick(ready_status());
    r.be.st.ifname = "ppp0";          // verhandelt, aber nie fertig

    for (int i = 0; i < 60; ++i) {
        if (r.link.tick(ready_status()).state == DataLinkState::Failed) break;
        r.clock.t += 1000;
    }
    TCHECK(r.link.state().state == DataLinkState::Failed);
    TCHECK(r.link.state().detail.find("haengt") != std::string::npos);
    // Und der haengende Prozess ist weg -- sonst haelt er den Modem-Port, und
    // der naechste Versuch diagnostiziert eine Datensession, die er selbst
    // hinterlassen hat.
    TCHECK(r.be.stops >= 1);
}

void test_an_unreadable_status_does_not_tear_down_a_running_call()
{
    // "Konnte nicht nachsehen" ist nicht "nichts laeuft". Die beiden zu
    // verwechseln hiesse, einen funktionierenden Anruf wegen einer
    // unlesbaren Datei zu beenden.
    Rig r;
    r.link.connect();
    r.link.tick(ready_status());
    TCHECK(r.be.starts == 1);

    r.be.readable = false;
    for (int i = 0; i < 5; ++i) { r.link.tick(ready_status()); r.clock.t += 500; }
    TCHECK(r.be.stops == 0);
    TCHECK(r.be.starts == 1);         // und kein zweiter pppd daneben
    TCHECK(r.link.state().state != DataLinkState::Failed);
}

void test_a_vanishing_modem_tears_the_call_down()
{
    Rig r;
    r.link.connect();
    r.link.tick(ready_status());
    r.be.negotiated("ppp0", "10.1.1.1", "10.1.1.2");
    r.clock.t += 500;
    TCHECK(r.link.tick(ready_status()).state == DataLinkState::Up);

    CellularStatus gone = ready_status();
    gone.present = false;
    r.clock.t += 500;
    TCHECK(r.link.tick(gone).state == DataLinkState::WaitDevice);
    TCHECK(r.be.stops >= 1);
    TCHECK(r.link.state().address.ipv4.empty());
}

void test_no_apn_is_refused_before_dialling()
{
    Rig r;
    CellularConfig c = ppp_config();
    c.apn.clear();
    r.link.set_config(c);
    r.link.connect();
    TCHECK(r.link.tick(ready_status()).state == DataLinkState::Failed);
    TCHECK(r.be.starts == 0);
    TCHECK(r.link.state().detail.find("APN") != std::string::npos);
}

void test_a_backend_that_cannot_start_is_a_failure_not_a_hang()
{
    Rig r;
    r.be.start_fails = true;
    r.link.connect();
    TCHECK(r.link.tick(ready_status()).state == DataLinkState::Failed);
    TCHECK(r.link.state().attempts == 1);
}

// ----------------------------------------------------------- Lebenszyklus --

void test_disconnect_is_idempotent_and_stops_the_process()
{
    Rig r;
    r.link.connect();
    r.link.tick(ready_status());
    r.be.negotiated("ppp0", "10.1.1.1", "10.1.1.2");
    r.clock.t += 500;
    r.link.tick(ready_status());

    r.link.disconnect();
    TCHECK(r.link.state().state == DataLinkState::Disabled);
    TCHECK(r.be.stops == 1);
    TCHECK(r.link.state().address.ipv4.empty());
    TCHECK(r.link.state().interface_name.empty());

    r.link.disconnect();
    r.link.disconnect();
    TCHECK(r.be.stops == 1);          // zweimal trennen tut nichts mehr
}

void test_repeated_connect_does_not_start_a_second_process()
{
    Rig r;
    r.link.connect();
    r.link.connect();
    r.link.connect();
    r.link.tick(ready_status());
    TCHECK(r.be.starts == 1);
    for (int i = 0; i < 5; ++i) { r.clock.t += 500; r.link.tick(ready_status()); }
    TCHECK(r.be.starts == 1);
}

void test_disconnect_while_negotiating_leaves_nothing_running()
{
    Rig r;
    r.link.connect();
    r.link.tick(ready_status());
    r.be.st.ifname = "ppp0";          // mitten in der Aushandlung
    r.clock.t += 500;
    r.link.tick(ready_status());

    r.link.disconnect();
    TCHECK(r.be.stops == 1);
    TCHECK(r.link.state().state == DataLinkState::Disabled);
    // Und ein Tick danach startet nichts neu.
    r.link.tick(ready_status());
    TCHECK(r.be.starts == 1);
}

void test_no_retry_storm_after_a_failure()
{
    Rig r;
    r.be.start_fails = true;
    r.link.connect();
    for (int i = 0; i < 100; ++i) { r.link.tick(ready_status()); r.clock.t += 100; }
    // Zehn Sekunden bei einem Backoff von 2/4/8/16/30 s sind eine Handvoll
    // Versuche. Hundert Takte duerfen nicht hundert Einwahlen bedeuten.
    TCHECK(r.be.starts > 0);
    TCHECK(r.be.starts < 10);
}

// ------------------------------------------- Trennung der beiden Pfade -----

void test_the_ppp_link_never_speaks_ecm()
{
    // Kein QNETDEVCTL, kein QCFG="usbnet", kein QCFG="nat". Waere eines davon
    // dabei, stellte der PPP-Pfad die USB-Komposition des Modems um -- und die
    // Einstellung ist im Modem persistent.
    Rig r;
    r.link.connect();
    for (int i = 0; i < 8; ++i) { r.link.tick(ready_status()); r.clock.t += 500; }
    TCHECK(r.at.count_sent("AT+QNETDEVCTL") == 0);
    TCHECK(r.at.count_sent("AT+QCFG=\"usbnet\"") == 0);
    TCHECK(r.at.count_sent("AT+QCFG=\"nat\"") == 0);
    TCHECK(r.at.count_sent("AT+CGCONTRDP") == 0);
}

void test_a_ppp_link_is_never_reported_as_dhcp()
{
    // Es gibt drei Wege zu einer Adresse, und PPP ist keiner davon mit DHCP:
    // sie kommt aus der IPCP-Aushandlung. Eine erste Fassung fragte nur nach
    // nic_mode -- und weil PppLink das Feld nie setzt, meldete jeder Anruf
    // dhcp=true. Wer dann sucht, warum "kein Lease" kommt, sucht nach einem
    // DHCP-Server, den es nie gab.
    ScriptedAtTransport at;
    FakePppBackend be;
    Clock clock;
    CellularService svc(at);
    PppLink link(at, be);
    link.set_clock([&clock] { return clock.t; });
    link.set_config(ppp_config());
    link.set_modem_port("/dev/ttyUSB4");
    at.reply("AT", "OK\r\n");
    at.reply("AT+CGACT=0,1", "OK\r\n");
    at.reply("AT+CGDCONT=1,\"IP\",\"internet.t-d1.de\"", "OK\r\n");

    net::CellularUplink uplink(svc, link);
    uplink.set_config(ppp_config());
    link.connect();
    link.tick(ready_status());
    be.negotiated("ppp0", "10.64.2.7", "10.64.2.8");
    clock.t += 500;
    link.tick(ready_status());
    uplink.tick();

    TCHECK(!uplink.info().dhcp);
    TCHECK(uplink.info().ifname == "ppp0");
}

void test_the_uplink_stays_logically_cellular()
{
    // Der Benutzer waehlt "Mobilfunk". Dass darunter PPP liegt, darf die ID
    // nicht aendern -- sonst passte eine Preference-Liste nach dem Umstellen
    // auf nichts mehr.
    ScriptedAtTransport at;
    FakePppBackend be;
    CellularService svc(at);
    PppLink link(at, be);
    net::CellularUplink uplink(svc, link);
    TCHECK(uplink.id() == "cellular");
    TCHECK(uplink.type() == net::UplinkType::Cellular);
}

// ------------------------------------------- Auswahl, Persistenz, Secrets --

void test_the_data_link_defaults_to_ecm()
{
    // PPP ist die Ausweichmoeglichkeit, nicht der zweite Hauptpfad. Eine
    // Kamera, die niemand konfiguriert hat, nimmt ECM -- so wie die Referenz
    // es auch tut ("Standard/Produktion" gegen "Kompatibilitaet").
    const CellularConfig fresh;
    TCHECK(fresh.data_link == "ecm");
    TCHECK(fresh.dial == "*99***1#");
}

void test_the_data_link_survives_a_settings_round_trip()
{
    CellularConfig c;
    c.data_link = "ppp";
    c.dial = "*99#";
    c.apn = "netpublic";
    std::vector<std::pair<std::string, std::string>> kv;
    api::cellular_config_to_settings(c, kv);

    CellularConfig back;
    std::string err;
    TCHECK(api::cellular_config_from_settings(kv, back, err));
    TCHECK(back.data_link == "ppp");
    TCHECK(back.dial == "*99#");
}

void test_an_unknown_data_link_is_refused_not_defaulted()
{
    // Stillschweigend auf ecm zurueckzufallen sieht aus wie "die Einstellung
    // hat nicht gegriffen" -- und der Bootpfad laedt dann andere Module, als
    // in der Datei stehen.
    CellularConfig c;
    std::string err;
    TCHECK(!api::cellular_config_from_settings({{"cellular.data_link", "pppoe"}}, c, err));
    TCHECK(!err.empty());
    TCHECK(c.data_link == "ecm");

    Json body; std::string e;
    TCHECK(Json::parse("{\"dataLink\":\"lte\"}", body, e));
    CellularConfig c2;
    TCHECK(!api::cellular_config_from_json(body, c2, e));
    TCHECK(c2.data_link == "ecm");

    TCHECK(Json::parse("{\"dataLink\":\"ppp\"}", body, e));
    TCHECK(api::cellular_config_from_json(body, c2, e));
    TCHECK(c2.data_link == "ppp");
}

void test_the_document_keeps_selection_and_reality_apart()
{
    // Zwischen dem Umstellen und dem Neustart laeuft noch der alte Datenlink.
    // Eine Oberflaeche, die nur eines von beiden kennt, sagt entweder "laeuft
    // schon" oder "ist aus" -- und beides waere falsch.
    CellularStatus st; st.present = true; st.responsive = true;
    CellularConfig cfg; cfg.data_link = "ppp";
    CellularLinkState link;                 // kind bleibt Ecm: das laeuft noch
    const std::string doc =
        api::cellular_network_json(st, cfg, link, net::LinkState::Down, false).dump();

    TCHECK(doc.find("\"kind\":\"ecm\"") != std::string::npos);
    TCHECK(doc.find("\"selected\":\"ppp\"") != std::string::npos);
    TCHECK(doc.find("\"rebootRequired\":true") != std::string::npos);

    // Und wenn beides uebereinstimmt, wird kein Neustart verlangt.
    link.kind = DataLinkKind::Ppp;
    const std::string same =
        api::cellular_network_json(st, cfg, link, net::LinkState::Connected, true).dump();
    TCHECK(same.find("\"kind\":\"ppp\"") != std::string::npos);
    TCHECK(same.find("\"rebootRequired\":false") != std::string::npos);
    // nicMode ist eine Aussage ueber ECM. Bei PPP gibt es keinen
    // Routing-Modus, und eine Angabe darueber waere erfunden.
    TCHECK(same.find("\"nicMode\":null") != std::string::npos);
}

void test_pap_or_chap_without_a_username_is_a_configuration_error()
{
    // Nicht still auf "keine Auth" zurueckfallen. Der Anruf scheiterte dann am
    // Netz, mit einer Meldung, die nichts ueber die Ursache sagt.
    Json body; std::string e;
    CellularConfig c;
    c.apn = "netpublic";

    TCHECK(Json::parse("{\"authMode\":\"pap\"}", body, e));
    TCHECK(!api::cellular_config_from_json(body, c, e));
    TCHECK(e.find("username") != std::string::npos);
    TCHECK(c.auth == AuthMode::None);       // nichts uebernommen

    TCHECK(Json::parse("{\"authMode\":\"chap\"}", body, e));
    TCHECK(!api::cellular_config_from_json(body, c, e));

    // Mit Benutzernamen geht es.
    TCHECK(Json::parse("{\"authMode\":\"pap\",\"username\":\"u\"}", body, e));
    TCHECK(api::cellular_config_from_json(body, c, e));
    TCHECK(c.auth == AuthMode::Pap);
}

void test_a_dial_number_cannot_break_out_of_the_chat_script()
{
    // Die Einwahlnummer landet im Chat-Skript zwischen EINFACHEN
    // Anfuehrungszeichen: OK 'ATD*99***1#'. Ein ' im Wert bricht dort aus und
    // macht aus dem Rest eigene chat-Woerter -- aus einem Feld in einem
    // Formular also eine Anweisung an den Wahlvorgang.
    //
    // Geprueft wird die Sperre selbst, nicht das Backend: LinuxPppBackend
    // laesst sich auf diesem Host nicht uebersetzen. Die Validierung muss also
    // schon greifen, bevor der Wert dorthin kommt.
    Json body; std::string e;
    CellularConfig c;
    const char* bad[] = {
        "{\"dial\":\"*99#' ; ATH ; '\"}",
        "{\"dial\":\"*99#\\\"\"}",
        "{\"dial\":\"*99#\\\\\"}",
    };
    for (const char* b : bad) {
        TCHECK(Json::parse(b, body, e));
        const std::string before = c.dial;
        TCHECK(!api::cellular_config_from_json(body, c, e));
        TCHECK(c.dial == before);
    }
    // Die normale Nummer bleibt erlaubt.
    TCHECK(Json::parse("{\"dial\":\"*99***1#\"}", body, e));
    TCHECK(api::cellular_config_from_json(body, c, e));
    TCHECK(c.dial == "*99***1#");
}

void test_no_secret_reaches_the_cellular_document()
{
    CellularStatus st; st.present = true;
    CellularConfig cfg;
    cfg.data_link = "ppp";
    cfg.password = "hunter2-apn-password";
    cfg.sim_pin = "4711";
    CellularLinkState link; link.kind = DataLinkKind::Ppp;
    const std::string doc =
        api::cellular_network_json(st, cfg, link, net::LinkState::Down, false).dump();
    TCHECK(doc.find("hunter2-apn-password") == std::string::npos);
    TCHECK(doc.find("4711") == std::string::npos);
    TCHECK(doc.find("\"passwordSet\":true") != std::string::npos);
    TCHECK(doc.find("\"simPinSet\":true") != std::string::npos);
}

} // namespace

void run_ppp_link_tests()
{
    test_ppp_is_not_started_until_someone_asks();
    test_the_link_says_which_kind_it_is();

    test_a_successful_dial_comes_up();
    test_the_interface_name_is_never_assumed();

    test_the_dial_waits_for_the_sim();
    test_the_dial_waits_for_registration();
    test_no_modem_port_is_a_wait_not_a_failure();
    test_an_authentication_failure_says_so();
    test_a_dial_that_never_connects_says_so();
    test_the_other_failures_are_each_their_own_sentence();
    test_a_hanging_negotiation_is_broken_off();
    test_an_unreadable_status_does_not_tear_down_a_running_call();
    test_a_vanishing_modem_tears_the_call_down();
    test_no_apn_is_refused_before_dialling();
    test_a_backend_that_cannot_start_is_a_failure_not_a_hang();

    test_disconnect_is_idempotent_and_stops_the_process();
    test_repeated_connect_does_not_start_a_second_process();
    test_disconnect_while_negotiating_leaves_nothing_running();
    test_no_retry_storm_after_a_failure();

    test_the_ppp_link_never_speaks_ecm();
    test_a_ppp_link_is_never_reported_as_dhcp();
    test_the_uplink_stays_logically_cellular();

    test_the_data_link_defaults_to_ecm();
    test_the_data_link_survives_a_settings_round_trip();
    test_an_unknown_data_link_is_refused_not_defaulted();
    test_the_document_keeps_selection_and_reality_apart();
    test_pap_or_chap_without_a_username_is_a_configuration_error();
    test_a_dial_number_cannot_break_out_of_the_chat_script();
    test_no_secret_reaches_the_cellular_document();
}
