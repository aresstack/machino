// Portzuordnung und AT-Rahmenerkennung.
//
// Beides ist reine Textverarbeitung und damit genau die Sorte Code, bei der
// ein Hardwaretest weniger beweist als ein Hosttest: am Modem sieht man EINE
// Verkabelung und EINE Antwort, hier sieht man die Faelle, die selten sind und
// dann wehtun.
#include "core/cellular/at_framing.hpp"
#include "core/cellular/modem_ports.hpp"
#include <cstdio>
#include <string>
#include <vector>

using namespace machino;
using namespace machino::cellular;

extern int g_fail_ext, g_pass_ext;
#define TCHECK(cond) do { if (cond) { ++g_pass_ext; } else { ++g_fail_ext; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

namespace {

SerialPortInfo port(const char* dev, const char* vid, const char* pid, int ifn)
{
    SerialPortInfo p;
    p.device = dev; p.vid = vid; p.pid = pid; p.interface_number = ifn;
    return p;
}

// Die Verkabelung, die das Referenzprojekt beschreibt: MI_00 Netzwerk (kein
// tty), MI_02 DIAG, MI_03 AT, MI_04 Modem.
void test_the_expected_ec200a_layout_maps_to_roles()
{
    const std::vector<SerialPortInfo> in = {
        port("/dev/ttyUSB0", "2c7c", "6005", 2),
        port("/dev/ttyUSB1", "2c7c", "6005", 3),
        port("/dev/ttyUSB2", "2c7c", "6005", 4),
    };
    const ModemPorts m = map_modem_ports(in);
    TCHECK(m.present);
    TCHECK(m.mapped_from_table);
    TCHECK(m.diag  == "/dev/ttyUSB0");
    TCHECK(m.at    == "/dev/ttyUSB1");
    TCHECK(m.modem == "/dev/ttyUSB2");
    TCHECK(m.all.size() == 3);
}

// Der eigentliche Zweck der Uebung: die ttyUSB-NUMMER darf nichts entscheiden.
//
// Haengt schon ein anderer serieller Adapter am Bus, faengt das Modem bei
// ttyUSB3 an. Wer /dev/ttyUSB2 fest verdrahtet, redet dann mit dem falschen
// Geraet -- und zwar mit einem, das vielleicht sogar antwortet.
void test_the_tty_number_decides_nothing()
{
    const std::vector<SerialPortInfo> in = {
        port("/dev/ttyUSB0", "0403", "6001", 0),   // ein FTDI, nicht unseres
        port("/dev/ttyUSB3", "2c7c", "6005", 2),
        port("/dev/ttyUSB4", "2c7c", "6005", 3),
        port("/dev/ttyUSB5", "2c7c", "6005", 4),
    };
    const ModemPorts m = map_modem_ports(in);
    TCHECK(m.at == "/dev/ttyUSB4");
    TCHECK(m.all.size() == 3);            // der FTDI gehoert nicht dazu
    for (const SerialPortInfo& p : m.all) TCHECK(p.vid == "2c7c");
}

// Reihenfolge im Verzeichnis ist beliebig; sysfs sortiert nicht fuer uns.
void test_order_of_discovery_does_not_matter()
{
    const std::vector<SerialPortInfo> in = {
        port("/dev/ttyUSB2", "2c7c", "6005", 4),
        port("/dev/ttyUSB0", "2c7c", "6005", 2),
        port("/dev/ttyUSB1", "2c7c", "6005", 3),
    };
    const ModemPorts m = map_modem_ports(in);
    TCHECK(m.at == "/dev/ttyUSB1");
    TCHECK(m.all[0].interface_number == 2);   // nach Interfacenummer sortiert
    TCHECK(m.all[2].interface_number == 4);
}

// Hex aus sysfs kommt klein; jemand koennte es gross hineinreichen.
void test_hex_case_does_not_matter()
{
    const std::vector<SerialPortInfo> in = { port("/dev/ttyUSB1", "2C7C", "6005", 3) };
    TCHECK(map_modem_ports(in).at == "/dev/ttyUSB1");
    TCHECK(map_modem_ports(in, "2C7C", "6005").at == "/dev/ttyUSB1");
}

// Kein Modem: kein Port, keine Rolle, und vor allem kein Raten.
void test_a_foreign_device_is_not_a_modem()
{
    const std::vector<SerialPortInfo> in = {
        port("/dev/ttyUSB0", "0403", "6001", 0),
        port("/dev/ttyUSB1", "10c4", "ea60", 0),
    };
    const ModemPorts m = map_modem_ports(in);
    TCHECK(!m.present);
    TCHECK(m.at.empty() && m.diag.empty() && m.modem.empty());
    TCHECK(m.all.empty());
}

// Ein Port, dessen Interfacenummer nicht lesbar war, ist DA -- aber er
// bekommt keine Rolle. Eine geratene Rolle hiesse: AT-Kommandos an den
// DIAG-Port, und der antwortet nicht so, wie irgendjemand erwartet.
void test_an_unknown_interface_gets_no_role()
{
    const std::vector<SerialPortInfo> in = {
        port("/dev/ttyUSB0", "2c7c", "6005", -1),
        port("/dev/ttyUSB1", "2c7c", "6005", 7),
    };
    const ModemPorts m = map_modem_ports(in);
    TCHECK(m.present);
    TCHECK(m.all.size() == 2);
    TCHECK(m.at.empty() && m.diag.empty() && m.modem.empty());
    TCHECK(!m.mapped_from_table);
}

// Zwei Ports auf derselben Interfacenummer sind ein Zustand, den wir nicht
// verstehen. Dann gewinnt der erste, statt dass die Rolle hin und her springt.
void test_a_duplicate_interface_does_not_overwrite()
{
    const std::vector<SerialPortInfo> in = {
        port("/dev/ttyUSB1", "2c7c", "6005", 3),
        port("/dev/ttyUSB9", "2c7c", "6005", 3),
    };
    const ModemPorts m = map_modem_ports(in);
    TCHECK(m.at == "/dev/ttyUSB1");
}

// ---------------------------------------------------------------- AT framing

void test_a_reply_ends_at_its_result_line()
{
    TCHECK(at_scan("") == AtResult::Pending);
    TCHECK(at_scan("AT\r\n") == AtResult::Pending);
    TCHECK(at_scan("AT\r\nOK\r\n") == AtResult::Ok);
    TCHECK(at_scan("ATI\r\nQuectel\r\nEC200A\r\n") == AtResult::Pending);
    TCHECK(at_scan("ATI\r\nQuectel\r\nEC200A\r\n\r\nOK\r\n") == AtResult::Ok);
    TCHECK(at_scan("AT+CPIN?\r\n+CME ERROR: 10\r\n") == AtResult::Error);
    TCHECK(at_scan("ATD*99#\r\nNO CARRIER\r\n") == AtResult::Error);
    TCHECK(at_scan("AT+X\r\nERROR\r\n") == AtResult::Error);
}

// Der Grund, warum die Ergebniszeile eine GANZE Zeile sein muss.
//
// Ein Teilstring-Vergleich beendet die Antwort mitten im Text, und was dann
// geparst wird, ist eine halbe Zeichenkette. Die Faelle unten sind nicht
// ausgedacht: Betreibernamen kommen roh aus dem Netz und duerfen alles
// enthalten, und die Modemantworten auf +CME sind laengere Saetze als der
// blosse Fehlercode.
//
// Nachgeprueft, dass dieser Test wirklich greift: mit at_scan auf
// `find("OK") != npos` umgebaut faellt er auf der COPS-Zeile um. Die
// Firmwarekennung "EC200AEUV1HAR02A07M16" steht hier als realistische lange
// Zeile -- sie enthaelt selbst KEIN Ergebniswort und faengt den Fehler
// deshalb auch nicht; das tut der Betreibername.
void test_a_result_word_inside_a_line_ends_nothing()
{
    TCHECK(at_scan("+COPS: 0,0,\"OK-Mobile\",7\r\n") == AtResult::Pending);
    TCHECK(at_scan("+COPS: 0,0,\"ERROR Telecom\",7\r\n") == AtResult::Pending);
    TCHECK(at_scan("+CME ERRORS ARE FUN\r\n") == AtResult::Pending);
    TCHECK(at_scan("NO CARRIERS HERE\r\n") == AtResult::Pending);
    TCHECK(at_scan("ATI\r\nEC200AEUV1HAR02A07M16\r\n") == AtResult::Pending);
    // und mit der echten Ergebniszeile dahinter zaehlt sie doch
    TCHECK(at_scan("ATI\r\nEC200AEUV1HAR02A07M16\r\nOK\r\n") == AtResult::Ok);
}

void test_payload_drops_the_echo_and_the_result_line()
{
    const std::string raw = "ATI\r\nQuectel\r\nEC200A\r\nEC200AEUV1HAR02A07M16\r\n\r\nOK\r\n";
    const std::string p = at_payload(raw, "ATI");
    TCHECK(p == "Quectel\nEC200A\nEC200AEUV1HAR02A07M16");

    // Ohne Echo (ATE0) fehlt nichts.
    TCHECK(at_payload("Quectel\r\nOK\r\n", "ATI") == "Quectel");
    // Die Fehlerzeile gehoert nicht in die Nutzlast; ihr Code kommt aus raw.
    TCHECK(at_payload("AT+CPIN?\r\n+CME ERROR: 10\r\n", "AT+CPIN?").empty());
}

} // namespace

void run_cellular_ports_tests()
{
    test_the_expected_ec200a_layout_maps_to_roles();
    test_the_tty_number_decides_nothing();
    test_order_of_discovery_does_not_matter();
    test_hex_case_does_not_matter();
    test_a_foreign_device_is_not_a_modem();
    test_an_unknown_interface_gets_no_role();
    test_a_duplicate_interface_does_not_overwrite();
    test_a_reply_ends_at_its_result_line();
    test_a_result_word_inside_a_line_ends_nothing();
    test_payload_drops_the_echo_and_the_result_line();
}
