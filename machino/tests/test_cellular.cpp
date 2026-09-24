// EC200A Control Plane: Parser, SIM-Schutz, Registrierung, Redaktion.
//
// Alles gegen aufgezeichnete Antworten. Das ist hier nicht die zweitbeste
// Loesung, sondern die einzig moegliche: eine SIM mit aufgebrauchten
// PIN-Versuchen laesst sich nicht herstellen, um sie zu testen, und genau
// dieser Fall ist der, bei dem ein Fehler die Karte des Besitzers kostet.
#include "core/cellular/at_parse.hpp"
#include "core/cellular/cellular_config.hpp"
#include "core/cellular/cellular_service.hpp"
#include "core/cellular/sim_manager.hpp"
#include "scripted_at_transport.hpp"
#include <cstdio>
#include <string>

using namespace machino;
using namespace machino::cellular;
using machino::test::ScriptedAtTransport;

extern int g_fail_ext, g_pass_ext;
#define TCHECK(cond) do { if (cond) { ++g_pass_ext; } else { ++g_fail_ext; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

namespace {

// ---------------------------------------------------------------- Fixtures

// Wie der EC200A wirklich antwortet, aus dem Referenzprojekt uebernommen:
// ATI liefert Hersteller, Modell und "Revision:".
const char* kAti =
    "ATI\r\n"
    "Quectel\r\n"
    "EC200A\r\n"
    "Revision: EC200AEUV1HAR02A07M16\r\n"
    "\r\nOK\r\n";

const char* kCgsn   = "AT+CGSN\r\n867725050123456\r\n\r\nOK\r\n";
const char* kQccid  = "AT+QCCID\r\n+QCCID: 89490200001234567890\r\n\r\nOK\r\n";
const char* kCimi   = "AT+CIMI\r\n262011234567890\r\n\r\nOK\r\n";
const char* kOk     = "OK\r\n";

const char* kCpinReady = "AT+CPIN?\r\n+CPIN: READY\r\n\r\nOK\r\n";
const char* kCpinPin   = "AT+CPIN?\r\n+CPIN: SIM PIN\r\n\r\nOK\r\n";
const char* kCpinPuk   = "AT+CPIN?\r\n+CPIN: SIM PUK\r\n\r\nOK\r\n";
const char* kCpinNoSim = "AT+CPIN?\r\n+CME ERROR: 10\r\n";

const char* kCeregHome    = "AT+CEREG?\r\n+CEREG: 2,1,\"1A2B\",\"01234567\",7\r\n\r\nOK\r\n";
const char* kCeregSearch  = "AT+CEREG?\r\n+CEREG: 2,2\r\n\r\nOK\r\n";
const char* kCeregDenied  = "AT+CEREG?\r\n+CEREG: 2,3\r\n\r\nOK\r\n";
const char* kCeregRoaming = "AT+CEREG?\r\n+CEREG: 2,5\r\n\r\nOK\r\n";

const char* kCsq  = "AT+CSQ\r\n+CSQ: 21,99\r\n\r\nOK\r\n";
const char* kCops = "AT+COPS?\r\n+COPS: 0,0,\"Telekom.de\",7\r\n\r\nOK\r\n";
const char* kQnw  = "AT+QNWINFO\r\n+QNWINFO: \"FDD LTE\",\"26201\",\"LTE BAND 3\",1300\r\n\r\nOK\r\n";

// QENG servingcell, LTE, vollstaendig. Indizes laut Referenzprojekt:
// 4=MCC 5=MNC 6=CellID 7=PCI 8=EARFCN 9=Band 12=TAC 13=RSRP 14=RSRQ 15=RSSI 16=SINR
const char* kQengFull =
    "AT+QENG=\"servingcell\"\r\n"
    "+QENG: \"servingcell\",\"NOCONN\",\"LTE\",\"FDD\",262,01,1A2B03,281,1300,3,5,5,1A2B,-95,-11,-65,12,-\r\n"
    "\r\nOK\r\n";

// Dieselbe Zeile, wie sie im Idle und bei schwachem Empfang aussieht: Luecken
// und "-" statt Zahlen. Das ist der Fall, in dem eine naive Auswertung Nullen
// erfindet.
const char* kQengSparse =
    "AT+QENG=\"servingcell\"\r\n"
    "+QENG: \"servingcell\",\"SEARCH\",\"LTE\",\"FDD\",,,,,,,,,,-,-,-,-\r\n"
    "\r\nOK\r\n";

const char* kCgpaddr = "AT+CGPADDR\r\n+CGPADDR: 1,\"10.12.34.56\"\r\n\r\nOK\r\n";

void arm_healthy(ScriptedAtTransport& t)
{
    t.reply("AT", kOk);
    t.reply("ATI", kAti);
    t.reply("AT+CGSN", kCgsn);
    t.reply("AT+CPIN?", kCpinReady);
    t.reply("AT+QCCID", kQccid);
    t.reply("AT+CIMI", kCimi);
    t.reply("AT+CEREG?", kCeregHome);
    t.reply("AT+CSQ", kCsq);
    t.reply("AT+COPS?", kCops);
    t.reply("AT+QNWINFO", kQnw);
    t.reply("AT+QENG=\"servingcell\"", kQengFull);
    t.reply("AT+CGPADDR", kCgpaddr);
}

// ----------------------------------------------------------------- Parser

void test_ati_gives_manufacturer_model_and_firmware()
{
    const ModemIdentity id = parse_ati(kAti);
    TCHECK(id.manufacturer == "Quectel");
    TCHECK(id.model == "EC200A");
    TCHECK(id.firmware == "EC200AEUV1HAR02A07M16");

    // Ohne Revision-Zeile bleibt die Firmware leer statt irgendetwas zu raten.
    const ModemIdentity partial = parse_ati("Quectel\r\nEC200A\r\nOK\r\n");
    TCHECK(partial.model == "EC200A" && partial.firmware.empty());
    // Und gar keine Antwort ergibt gar keine Felder.
    const ModemIdentity none = parse_ati("");
    TCHECK(none.manufacturer.empty() && none.model.empty());
}

void test_bare_numeric_answers_survive_the_echo()
{
    // IMEI und IMSI kommen ohne Prefix. Vor ihnen steht das Echo, das
    // ebenfalls kein Prefix hat -- die Ziffernregel unterscheidet sie.
    TCHECK(first_numeric_line(kCgsn) == "867725050123456");
    TCHECK(first_numeric_line(kCimi) == "262011234567890");
    TCHECK(first_numeric_line("AT+CGSN\r\nERROR\r\n").empty());
    // Zu kurz ist keine IMEI.
    TCHECK(first_numeric_line("12345\r\n").empty());
}

void test_csv_fields_respect_quotes()
{
    // Ein Betreibername darf ein Komma enthalten; der Standard verbietet es
    // nicht, und dann verschiebt sich ohne Quoting jedes folgende Feld.
    const std::string line = "0,0,\"Mobil, GmbH\",7";
    TCHECK(csv_field(line, 2) == "Mobil, GmbH");
    TCHECK(csv_field(line, 3) == "7");
    TCHECK(csv_field(line, 9).empty());
}

void test_cereg_is_a_state_not_a_boolean()
{
    TCHECK(parse_cereg(kCeregHome)    == RegState::RegisteredHome);
    TCHECK(parse_cereg(kCeregSearch)  == RegState::Searching);
    TCHECK(parse_cereg(kCeregDenied)  == RegState::Denied);
    TCHECK(parse_cereg(kCeregRoaming) == RegState::RegisteredRoaming);
    TCHECK(parse_cereg("+CEREG: 2,0\r\nOK\r\n") == RegState::NotRegistered);
    // 4 heisst im Standard ausdruecklich "unknown".
    TCHECK(parse_cereg("+CEREG: 2,4\r\nOK\r\n") == RegState::Unknown);
    TCHECK(parse_cereg("ERROR\r\n") == RegState::Unknown);
    TCHECK(parse_cereg("") == RegState::Unknown);

    // Der Unterschied, um den es geht: "wird gesucht" und "verweigert" sind
    // beide nicht registriert, verlangen aber verschiedene Reaktionen.
    TCHECK(!reg_is_registered(RegState::Searching));
    TCHECK(!reg_is_registered(RegState::Denied));
    TCHECK(reg_is_registered(RegState::RegisteredRoaming));
}

void test_csq_99_is_not_a_measurement()
{
    const SignalInfo good = parse_csq(kCsq);
    TCHECK(good.csq.has && good.csq.value == 21);
    TCHECK(good.rssi_dbm.has && good.rssi_dbm.value == -71);   // -113 + 2*21

    // 99 heisst "nicht bekannt". Als Zahl durchgereicht waere es -113+198 =
    // +85 dBm, also ein Sender neben dem Ohr.
    const SignalInfo unknown = parse_csq("+CSQ: 99,99\r\nOK\r\n");
    TCHECK(!unknown.csq.has);
    TCHECK(!unknown.rssi_dbm.has);
    TCHECK(!parse_csq("ERROR\r\n").csq.has);
}

void test_qeng_missing_values_stay_missing()
{
    const ServingCell full = parse_qeng_servingcell(kQengFull);
    TCHECK(full.state == "NOCONN" && full.rat == "LTE");
    TCHECK(full.mcc == "262" && full.mnc == "01");
    TCHECK(full.pci.has && full.pci.value == 281);
    TCHECK(full.earfcn.has && full.earfcn.value == 1300);
    TCHECK(full.band.has && full.band.value == 3);
    TCHECK(full.band_mhz.has && full.band_mhz.value == 1800);
    TCHECK(full.tac == "1A2B");
    TCHECK(full.rsrp.has && full.rsrp.value == -95);
    TCHECK(full.rsrq.has && full.rsrq.value == -11);
    TCHECK(full.rssi.has && full.rssi.value == -65);
    TCHECK(full.sinr.has && full.sinr.value == 12);

    // Der Fall, der zaehlt: leere Felder und "-" duerfen KEINE Nullen werden.
    const ServingCell sparse = parse_qeng_servingcell(kQengSparse);
    TCHECK(sparse.state == "SEARCH");
    TCHECK(!sparse.rsrp.has && !sparse.rsrq.has && !sparse.rssi.has && !sparse.sinr.has);
    TCHECK(!sparse.pci.has && !sparse.earfcn.has && !sparse.band.has);
    TCHECK(!sparse.band_mhz.has);

    // Eine Nachbarzellenzeile ist keine Serving-Cell-Zeile.
    const ServingCell wrong = parse_qeng_servingcell(
        "+QENG: \"neighbourcell intra\",\"LTE\",1300,281,-95\r\nOK\r\n");
    TCHECK(wrong.rat.empty() && !wrong.rsrp.has);
}

void test_an_inactive_context_has_no_address()
{
    TCHECK(parse_cgpaddr(kCgpaddr).ipv4 == "10.12.34.56");
    // 0.0.0.0 ist die Abwesenheit einer Adresse, nicht eine.
    TCHECK(parse_cgpaddr("+CGPADDR: 1,\"0.0.0.0\"\r\nOK\r\n").ipv4.empty());
    TCHECK(parse_cgpaddr("ERROR\r\n").ipv4.empty());
}

void test_sim_pin2_is_not_a_pin_prompt()
{
    // Der gefaehrlichste Parserfehler in dieser Datei: SIM PIN2 enthaelt
    // "SIM PIN". Wer darauf mit der konfigurierten PIN antwortet, verbrennt
    // einen Versuch der FALSCHEN PIN -- PIN2 ist ein anderes Geheimnis.
    TCHECK(parse_cpin(kCpinReady) == SimState::Ready);
    TCHECK(parse_cpin(kCpinPin)   == SimState::PinRequired);
    TCHECK(parse_cpin(kCpinPuk)   == SimState::PukRequired);
    TCHECK(parse_cpin(kCpinNoSim) == SimState::NotInserted);
    TCHECK(parse_cpin("+CPIN: SIM PIN2\r\nOK\r\n") == SimState::Unknown);
    TCHECK(parse_cpin("+CPIN: SIM PUK2\r\nOK\r\n") == SimState::Unknown);
    TCHECK(parse_cpin("") == SimState::Unknown);
}

// -------------------------------------------------------------- SIM-Schutz

void test_a_ready_sim_is_never_sent_a_pin()
{
    ScriptedAtTransport t;
    t.reply("AT+CPIN?", kCpinReady);
    SimManager sim;
    const SimSnapshot s = sim.ensure_ready(t, "1234");
    TCHECK(s.state == SimState::Ready);
    TCHECK(t.count_sent("AT+CPIN=") == 0);
}

void test_a_puk_locked_sim_is_never_sent_a_pin()
{
    ScriptedAtTransport t;
    t.reply("AT+CPIN?", kCpinPuk);
    SimManager sim;
    const SimSnapshot s = sim.ensure_ready(t, "1234");
    TCHECK(s.state == SimState::PukRequired);
    TCHECK(t.count_sent("AT+CPIN=") == 0);
    TCHECK(s.detail.find("PUK") != std::string::npos);
}

void test_the_pin_goes_out_exactly_once_and_never_again()
{
    // Das ist der Test, um den es in diesem AP geht.
    ScriptedAtTransport t;
    t.reply("AT+CPIN?", kCpinPin);          // bleibt stehen: SIM sagt weiter PIN
    t.reply("AT+CPIN=\"1234\"", "+CME ERROR: 16\r\n");   // falsche PIN

    SimManager sim;
    const SimSnapshot first = sim.ensure_ready(t, "1234");
    TCHECK(first.pin_attempted && first.pin_rejected);
    TCHECK(t.count_sent("AT+CPIN=") == 1);

    // Ein Supervisor, der es hundertmal versucht, darf die Karte nicht
    // sperren. Drei falsche Versuche reichen fuer den PUK.
    for (int i = 0; i < 100; ++i) sim.ensure_ready(t, "1234");
    TCHECK(t.count_sent("AT+CPIN=") == 1);
    TCHECK(sim.last().pin_rejected);
    TCHECK(sim.last().detail.find("PUK-Schutz") != std::string::npos);
}

void test_a_corrected_pin_may_try_again()
{
    ScriptedAtTransport t;
    t.reply("AT+CPIN?", kCpinPin);
    t.reply("AT+CPIN=\"1234\"", "+CME ERROR: 16\r\n");
    t.reply("AT+CPIN=\"4321\"", kOk);

    SimManager sim;
    sim.ensure_ready(t, "1234");
    TCHECK(t.count_sent("AT+CPIN=") == 1);
    sim.ensure_ready(t, "1234");
    TCHECK(t.count_sent("AT+CPIN=") == 1);      // dieselbe: nein

    // Der Benutzer korrigiert die PIN. Ohne diesen Weg muesste er neu starten.
    sim.forget_attempt();
    sim.ensure_ready(t, "4321");
    TCHECK(t.count_sent("AT+CPIN=") == 2);
}

void test_the_block_survives_a_process_restart()
{
    // Auf dem ESP32 heisst "je Boot" dasselbe wie "je Prozess". Auf Linux
    // nicht: ein abgestuerzter Supervisor faengt von vorn an und schickt
    // dieselbe abgelehnte PIN erneut. Dreimal, und die Karte will den PUK.
    std::string persisted;
    auto load  = [&](std::string& tok) { if (persisted.empty()) return false; tok = persisted; return true; };
    auto store = [&](const std::string& tok) { persisted = tok; };

    ScriptedAtTransport t;
    t.reply("AT+CPIN?", kCpinPin);
    t.reply("AT+CPIN=\"1234\"", "+CME ERROR: 16\r\n");

    {
        SimManager sim;
        sim.set_persistence(load, store);
        sim.ensure_ready(t, "1234");
    }
    TCHECK(t.count_sent("AT+CPIN=") == 1);
    TCHECK(!persisted.empty());

    // Neuer Prozess, frischer SimManager, dieselbe Notiz.
    {
        SimManager sim;
        sim.set_persistence(load, store);
        const SimSnapshot s = sim.ensure_ready(t, "1234");
        TCHECK(s.pin_rejected);
    }
    TCHECK(t.count_sent("AT+CPIN=") == 1);
}

void test_the_persisted_note_does_not_contain_the_pin()
{
    const std::string tok = sim_attempt_token("1234");
    TCHECK(!tok.empty());
    TCHECK(tok.find("1234") == std::string::npos);
    // Verschiedene PINs, verschiedene Notizen -- sonst wuerde eine korrigierte
    // PIN faelschlich als schon abgelehnt gelten.
    TCHECK(sim_attempt_token("1234") != sim_attempt_token("4321"));
    TCHECK(sim_attempt_token("").empty());
}

void test_no_pin_configured_means_no_command()
{
    ScriptedAtTransport t;
    t.reply("AT+CPIN?", kCpinPin);
    SimManager sim;
    const SimSnapshot s = sim.ensure_ready(t, "");
    TCHECK(s.state == SimState::PinRequired);
    TCHECK(t.count_sent("AT+CPIN=") == 0);
    TCHECK(s.detail.find("keine konfiguriert") != std::string::npos);
}

// ------------------------------------------------------------- Redaktion

void test_secrets_never_reach_a_log_line()
{
    TCHECK(redact_at("AT+CPIN=\"1234\"") == "AT+CPIN=<redigiert>");
    TCHECK(redact_at("AT+CPIN=\"1234\"").find("1234") == std::string::npos);
    TCHECK(redact_at("AT+CLCK=\"SC\",1,\"1234\"").find("1234") == std::string::npos);
    TCHECK(redact_at("AT+CPWD=\"SC\",\"1234\",\"4321\"").find("1234") == std::string::npos);
    TCHECK(redact_at("AT+QICSGP=1,1,\"apn\",\"user\",\"geheim\",2").find("geheim") == std::string::npos);
    TCHECK(redact_at("AT+CGAUTH=1,2,\"user\",\"geheim\"").find("geheim") == std::string::npos);
    // Harmlose Kommandos bleiben lesbar -- sonst ist das Log wertlos.
    TCHECK(redact_at("AT+CPIN?") == "AT+CPIN?");
    TCHECK(redact_at("AT+CEREG?") == "AT+CEREG?");
    TCHECK(redact_at("ATI") == "ATI");
}

// ----------------------------------------------------------------- Dienst

void test_a_healthy_modem_fills_the_whole_model()
{
    ScriptedAtTransport t;
    arm_healthy(t);
    CellularService svc(t);
    const CellularStatus& s = svc.poll();

    TCHECK(s.present && s.responsive);
    TCHECK(s.identity.model == "EC200A");
    TCHECK(s.imei == "867725050123456");
    TCHECK(s.sim == SimState::Ready);
    TCHECK(s.iccid == "89490200001234567890");
    TCHECK(s.imsi == "262011234567890");
    TCHECK(s.registration == RegState::RegisteredHome);
    TCHECK(s.operator_name == "Telekom.de");
    TCHECK(s.operator_code == "26201");
    TCHECK(s.rat == "FDD LTE");
    TCHECK(s.cell.band.has && s.cell.band.value == 3);
    TCHECK(s.signal.rssi_dbm.has);
    TCHECK(s.pdp.ipv4 == "10.12.34.56");
    TCHECK(s.last_error.empty());
}

void test_no_port_is_reported_as_such_without_talking()
{
    ScriptedAtTransport t;
    t.set_available(false);
    CellularService svc(t);
    const CellularStatus& s = svc.poll();
    TCHECK(!s.present && !s.responsive);
    TCHECK(t.sent().empty());        // kein Kommando an ein Geraet, das fehlt
    TCHECK(!s.last_error.empty());
}

void test_a_silent_modem_is_distinguished_from_a_missing_one()
{
    ScriptedAtTransport t;
    t.set_timeout("AT");
    CellularService svc(t);
    const CellularStatus& s = svc.poll();
    TCHECK(s.present);              // der Port ist da
    TCHECK(!s.responsive);          // aber niemand antwortet
    TCHECK(s.last_error.find("keine Antwort") != std::string::npos);
}

void test_an_unregistered_modem_reports_no_stale_radio_values()
{
    // Ohne Registrierung ist ein Betreibername der zuletzt gesehene, nicht
    // der aktuelle. Ihn trotzdem anzuzeigen waere eine Falschaussage.
    ScriptedAtTransport t;
    arm_healthy(t);
    ScriptedAtTransport t2;
    t2.reply("AT", kOk);
    t2.reply("ATI", kAti);
    t2.reply("AT+CGSN", kCgsn);
    t2.reply("AT+CPIN?", kCpinReady);
    t2.reply("AT+QCCID", kQccid);
    t2.reply("AT+CIMI", kCimi);
    t2.reply("AT+CEREG?", kCeregSearch);
    t2.reply("AT+CSQ", kCsq);

    CellularService svc(t2);
    const CellularStatus& s = svc.poll();
    TCHECK(s.registration == RegState::Searching);
    TCHECK(s.operator_name.empty());
    TCHECK(s.rat.empty());
    TCHECK(!s.cell.rsrp.has);
    TCHECK(t2.count_sent("AT+COPS?") == 0);     // gar nicht erst gefragt
    TCHECK(s.last_error.find("searching") != std::string::npos);
}

void test_a_locked_sim_stops_the_round_early()
{
    ScriptedAtTransport t;
    t.reply("AT", kOk);
    t.reply("ATI", kAti);
    t.reply("AT+CGSN", kCgsn);
    t.reply("AT+CPIN?", kCpinPuk);
    t.reply("AT+QCCID", kQccid);

    CellularService svc(t);
    const CellularStatus& s = svc.poll();
    TCHECK(s.sim == SimState::PukRequired);
    TCHECK(s.iccid == "89490200001234567890");   // ICCID geht ohne Entsperren
    TCHECK(s.imsi.empty());                      // IMSI nicht
    TCHECK(t.count_sent("AT+CIMI") == 0);
    TCHECK(t.count_sent("AT+CEREG?") == 0);
}

void test_the_service_changes_nothing_persistent()
{
    // AP-M3 ist Control Plane. Kommandos, die die USB-Komposition umstellen
    // oder das Modem neu starten, gehoeren zum Verbindungsaufbau -- ein
    // Statusdienst, der das beim Start tut, ist eine Ueberraschung.
    ScriptedAtTransport t;
    arm_healthy(t);
    CellularService svc(t);
    svc.poll();
    svc.poll();
    for (const std::string& c : t.sent()) {
        TCHECK(c.find("QCFG") == std::string::npos);
        TCHECK(c.find("CFUN") == std::string::npos);
        TCHECK(c.find("QNETDEVCTL") == std::string::npos);
        TCHECK(c.compare(0, 3, "ATD") != 0);
        TCHECK(c.find("CGDCONT") == std::string::npos);
    }
}

void test_identity_is_read_once_not_every_round()
{
    ScriptedAtTransport t;
    arm_healthy(t);
    CellularService svc(t);
    svc.poll();
    const int after_first = t.count_sent("ATI");
    for (int i = 0; i < 5; ++i) svc.poll();
    TCHECK(after_first == 1);
    TCHECK(t.count_sent("ATI") == 1);
    TCHECK(t.count_sent("AT+CGSN") == 1);
    // ...und die Werte bleiben trotzdem im Modell stehen.
    TCHECK(svc.status().identity.model == "EC200A");
    TCHECK(svc.status().imei == "867725050123456");
}

void test_changing_the_pin_clears_the_block()
{
    ScriptedAtTransport t;
    t.reply("AT", kOk);
    t.reply("ATI", kAti);
    t.reply("AT+CGSN", kCgsn);
    t.reply("AT+CPIN?", kCpinPin);
    t.reply("AT+CPIN=\"1111\"", "+CME ERROR: 16\r\n");
    t.reply("AT+CPIN=\"2222\"", "+CME ERROR: 16\r\n");

    CellularConfig c;
    c.sim_pin = "1111";
    CellularService svc(t);
    svc.set_config(c);
    svc.poll();
    svc.poll();
    TCHECK(t.count_sent("AT+CPIN=") == 1);

    c.sim_pin = "2222";
    svc.set_config(c);          // neue PIN -> ein neuer Versuch ist erlaubt
    svc.poll();
    TCHECK(t.count_sent("AT+CPIN=") == 2);
    svc.poll();
    TCHECK(t.count_sent("AT+CPIN=") == 2);   // und wieder Schluss
}

// ------------------------------------------------------------- Konfiguration

void test_config_values_round_trip_by_name()
{
    PdpType p; AuthMode a;
    TCHECK(pdp_type_parse("IP", p) && p == PdpType::Ipv4);
    TCHECK(pdp_type_parse("IPV4V6", p) && p == PdpType::Ipv4v6);
    TCHECK(!pdp_type_parse("IPV6", p));            // das Modem kann es nicht allein
    TCHECK(std::string(pdp_type_name(PdpType::Ipv4)) == "IP");

    TCHECK(auth_mode_parse("none", a) && a == AuthMode::None);
    TCHECK(auth_mode_parse("pap", a)  && a == AuthMode::Pap);
    TCHECK(auth_mode_parse("chap", a) && a == AuthMode::Chap);
    TCHECK(!auth_mode_parse("kerberos", a));
}

void test_presets_are_suggestions_with_the_reason_attached()
{
    const auto& ps = apn_presets();
    TCHECK(ps.size() >= 2);
    bool telekom = false, o2 = false;
    for (const ApnPreset& p : ps) {
        if (std::string(p.apn) == "internet.t-d1.de") {
            telekom = true;
            // PDP IP, nicht IPV4V6 -- das ist der Punkt an dieser Konfiguration.
            TCHECK(p.pdp == PdpType::Ipv4);
            TCHECK(p.auth == AuthMode::None);
            TCHECK(std::string(p.note).find("CGNAT") != std::string::npos);
        }
        if (std::string(p.apn) == "netpublic") o2 = true;
    }
    TCHECK(telekom && o2);

    // Nichts davon ist Default: machino weiss nicht, welche SIM steckt.
    const CellularConfig fresh;
    TCHECK(fresh.apn.empty());
    TCHECK(!fresh.enabled);
    TCHECK(fresh.pdp == PdpType::Ipv4);
    TCHECK(fresh.auth == AuthMode::None);
}

} // namespace

void run_cellular_tests()
{
    test_ati_gives_manufacturer_model_and_firmware();
    test_bare_numeric_answers_survive_the_echo();
    test_csv_fields_respect_quotes();
    test_cereg_is_a_state_not_a_boolean();
    test_csq_99_is_not_a_measurement();
    test_qeng_missing_values_stay_missing();
    test_an_inactive_context_has_no_address();
    test_sim_pin2_is_not_a_pin_prompt();

    test_a_ready_sim_is_never_sent_a_pin();
    test_a_puk_locked_sim_is_never_sent_a_pin();
    test_the_pin_goes_out_exactly_once_and_never_again();
    test_a_corrected_pin_may_try_again();
    test_the_block_survives_a_process_restart();
    test_the_persisted_note_does_not_contain_the_pin();
    test_no_pin_configured_means_no_command();

    test_secrets_never_reach_a_log_line();

    test_a_healthy_modem_fills_the_whole_model();
    test_no_port_is_reported_as_such_without_talking();
    test_a_silent_modem_is_distinguished_from_a_missing_one();
    test_an_unregistered_modem_reports_no_stale_radio_values();
    test_a_locked_sim_stops_the_round_early();
    test_the_service_changes_nothing_persistent();
    test_identity_is_read_once_not_every_round();
    test_changing_the_pin_clears_the_block();

    test_config_values_round_trip_by_name();
    test_presets_are_suggestions_with_the_reason_attached();
}
