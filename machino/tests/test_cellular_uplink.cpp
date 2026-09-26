// AP-M5: Mobilfunk als ganz gewoehnlicher Uplink.
//
// Die Faelle hier pruefen vor allem, was NICHT passieren darf. Ein Uplink, der
// zu frueh "verbunden" sagt, ist schlimmer als einer, der gar nichts sagt: das
// Failover schaltet dann auf ihn um, der Verkehr versickert, und die
// Statusseite behauptet, alles sei in Ordnung.
#include "core/net/cellular_uplink.hpp"
#include "core/net/connectivity.hpp"
#include "scripted_at_transport.hpp"
#include <cstdio>
#include <string>
#include <vector>

using namespace machino;
using namespace machino::net;
using machino::test::ScriptedAtTransport;

extern int g_fail_ext, g_pass_ext;
#define TCHECK(cond) do { if (cond) { ++g_pass_ext; } else { ++g_fail_ext; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

namespace {

// Dasselbe Linux-Doppel wie in test_ecm_link, auf das Noetige gekuerzt.
class FakeEcmBackend : public IEcmBackend {
public:
    bool        iface_present = false;
    std::string iface_name = "usb0";
    bool        dhcp_gives_address = true;
    int         teardowns = 0;
    LinkAddress assigned;

    bool find_interface(EcmInterface& out) override
    {
        if (!iface_present) return false;
        out.name = iface_name; out.up = up_; out.carrier = true;
        return true;
    }
    bool set_up(const std::string&, bool up) override { up_ = up; return true; }
    bool dhcp_start(const std::string&) override { running_ = true; return true; }
    bool dhcp_stop(const std::string&) override { running_ = false; return true; }
    bool read_address(const std::string&, LinkAddress& out) override
    {
        if (assigned.has_address()) { out = assigned; return true; }
        if (!running_ || !dhcp_gives_address) return false;
        out.ipv4 = "192.168.43.100"; out.gateway = "192.168.43.1"; out.dns1 = "192.168.43.1";
        return true;
    }
    bool set_address(const std::string&, const LinkAddress& a) override
    {
        assigned = a;
        return true;
    }
    void teardown(const std::string&) override { ++teardowns; assigned = LinkAddress{}; }

private:
    bool up_ = false;
    bool running_ = false;
};

struct Clock { uint64_t t = 1000; };

// Ein Modem, das auf alles Noetige antwortet und im NIC-Modus eine oeffentliche
// Adresse liefert. Dieselben Antworten wie in test_ecm_link, damit beide
// Dateien dasselbe Geraet beschreiben.
void arm_healthy_modem(ScriptedAtTransport& t)
{
    // Der blanke Ping zuerst: CellularService fragt damit, ob ueberhaupt
    // jemand da ist, und ohne Antwort darauf kommt es gar nicht zur SIM.
    t.reply("AT", "OK\r\n");
    t.reply("ATI", "Quectel\r\nEC200A\r\nRevision: EC200AEUHAR03A04M16\r\nOK\r\n");
    t.reply("AT+CPIN?", "+CPIN: READY\r\nOK\r\n");
    t.reply("AT+CEREG?", "+CEREG: 0,1\r\nOK\r\n");
    t.reply("AT+CSQ", "+CSQ: 22,99\r\nOK\r\n");
    t.reply("AT+COPS?", "+COPS: 0,0,\"Telekom.de\",7\r\nOK\r\n");
    t.reply("AT+QCFG=\"usbnet\"", "+QCFG: \"usbnet\",1\r\nOK\r\n");
    t.reply("AT+QCFG=\"nat\"", "+QCFG: \"nat\",1\r\nOK\r\n");
    t.reply("AT+CGDCONT=1,\"IP\",\"internet.t-d1.de\"", "OK\r\n");
    t.reply("AT+QICSGP=1,1,\"internet.t-d1.de\",\"\",\"\",0", "OK\r\n");
    t.reply("AT+QNETDEVCTL=3,1", "OK\r\n");
    t.reply("AT+CGCONTRDP=1",
            "+CGCONTRDP: 1,5,\"internet.t-d1.de\",\"37.81.97.187.255.255.255.240\","
            "\"37.81.97.185\",\"10.74.210.210\",\"10.74.210.211\"\r\nOK\r\n");
}

cellular::CellularConfig on_config()
{
    cellular::CellularConfig c;
    c.enabled = true;
    c.auto_connect = true;
    c.apn = "internet.t-d1.de";
    c.nic_mode = true;
    return c;
}

// Ein zusammengestecktes Mobilfunk-Subsystem, wie es in main.cpp entsteht.
struct Rig {
    ScriptedAtTransport at;
    FakeEcmBackend      be;
    Clock               clock;
    cellular::CellularService svc{at};
    cellular::EcmLink         link{at, be};
    CellularUplink            uplink{svc, link};

    Rig()
    {
        svc.set_clock([this] { return clock.t; });
        link.set_clock([this] { return clock.t; });
    }
    // Ueber den Uplink, nicht an ihm vorbei -- so herum macht es main.cpp auch,
    // und nur so wird die Absicht im naechsten tick() wirklich durchgesetzt.
    void configure(const cellular::CellularConfig& c) { uplink.set_config(c); }
    void run(int n = 14, uint64_t step = 500)
    {
        for (int i = 0; i < n; ++i) {
            uplink.tick();
            if (uplink.state() == LinkState::Connected) return;
            clock.t += step;
        }
    }
};

// ---------------------------------------------------------------- Identitaet

void test_the_id_is_cellular_and_not_the_interface_name()
{
    // Eine Preference-Liste, die "usb0" enthielte, waere nach einem
    // Treiberwechsel still wirkungslos -- der Eintrag passt auf nichts mehr,
    // und niemand bekaeme es gesagt.
    Rig r;
    r.be.iface_present = true;
    r.be.iface_name = "eth1";           // cdc_ether hat es so genannt
    r.configure(on_config());
    arm_healthy_modem(r.at);
    r.run();

    TCHECK(r.uplink.state() == LinkState::Connected);
    TCHECK(r.uplink.id() == "cellular");
    TCHECK(r.uplink.info().ifname == "eth1");     // der Name steht im Info, nicht in der ID
    TCHECK(r.uplink.type() == UplinkType::Cellular);
}

void test_the_id_survives_an_interface_rename()
{
    Rig r;
    r.be.iface_present = true; r.be.iface_name = "usb0";
    r.configure(on_config());
    arm_healthy_modem(r.at);
    r.run();
    const std::string before = r.uplink.id();

    // Das Modem kommt nach einem Neustart als wwan0 wieder.
    r.link.disconnect();
    r.be.iface_name = "wwan0";
    r.at.set_reply("AT+CGCONTRDP=1",
                   "+CGCONTRDP: 1,5,\"internet.t-d1.de\",\"37.81.97.187.255.255.255.240\","
                   "\"37.81.97.185\",\"10.74.210.210\"\r\nOK\r\n");
    r.link.connect();
    r.run();

    TCHECK(r.uplink.id() == before);
    TCHECK(r.uplink.info().ifname == "wwan0");
}

void test_a_second_modem_can_carry_its_own_id()
{
    ScriptedAtTransport at; FakeEcmBackend be;
    cellular::CellularService svc(at);
    cellular::EcmLink link(at, be);
    CellularUplink u(svc, link, "lte1");
    TCHECK(u.id() == "lte1");

    // Ein leerer Name waere ein Uplink, den keine Policy ansprechen kann.
    CellularUplink empty(svc, link, "");
    TCHECK(empty.id() == "cellular");
}

// --------------------------------------------------------- die sechs Stufen

void test_no_modem_is_absent_not_failed()
{
    Rig r;
    r.configure(on_config());
    r.at.set_available(false);      // kein AT-Port
    r.uplink.tick();
    TCHECK(r.uplink.state() == LinkState::Absent);
    TCHECK(!r.uplink.has_internet());
}

void test_switched_off_is_absent_and_sends_nothing()
{
    // Aus heisst aus. Ein Statusdienst, der ein abgeschaltetes Modem trotzdem
    // im Sekundentakt abfragt, haelt den AT-Port belegt und die Kamera wach.
    Rig r;
    cellular::CellularConfig c = on_config();
    c.enabled = false;
    r.configure(c);
    arm_healthy_modem(r.at);

    for (int i = 0; i < 5; ++i) r.uplink.tick();
    TCHECK(r.uplink.state() == LinkState::Absent);
    TCHECK(r.at.sent().empty());
    TCHECK(!r.uplink.enabled());
}

void test_enabled_is_the_switch_even_with_auto_connect_off()
{
    // autoConnect is false by DEFAULT. A first version made the bring-up
    // depend on it, so switching cellular on did nothing whatsoever -- no
    // error, no hint, and no second button that could have triggered it.
    // autoConnect describes something else: the modem's own persistent
    // self-connect (AT+QNETDEVCTL type 3), which belongs to the state machine.
    Rig r;
    r.be.iface_present = true;
    cellular::CellularConfig c = on_config();
    c.auto_connect = false;
    r.configure(c);
    arm_healthy_modem(r.at);
    r.run();
    TCHECK(r.uplink.state() == LinkState::Connected);
}

void test_a_modem_that_enumerates_but_does_not_answer_is_down()
{
    Rig r;
    r.configure(on_config());
    // Ein AT-Port existiert (present), aber es kommt nichts zurueck. Das ist
    // ein anderer Fehler als "kein Modem", und die Anzeige muss ihn anders
    // nennen koennen.
    r.at.set_reply("ATI", "");
    r.svc.poll();
    r.uplink.tick();
    TCHECK(r.uplink.state() != LinkState::Connected);
    TCHECK(!r.uplink.has_internet());
}

void test_registered_but_no_datalink_is_not_connected()
{
    // Die Stufe, an der ein naiver Adapter "verbunden" sagt: SIM bereit, im
    // Netz, Funk gut -- und kein einziges Byte kann irgendwohin.
    Rig r;
    r.configure(on_config());
    arm_healthy_modem(r.at);
    r.at.set_reply("AT+QNETDEVCTL=3,1", "ERROR\r\n");
    r.run(6);

    TCHECK(r.uplink.state() != LinkState::Connected);
    TCHECK(!r.uplink.has_internet());
    TCHECK(r.uplink.info().ipv4.empty());
}

void test_an_interface_without_an_address_is_not_connected()
{
    // "Es gibt ein usb0" ist keine Verbindung. Genau diese Verwechslung
    // verbietet AP-M5 ausdruecklich.
    Rig r;
    r.be.iface_present = true;
    r.be.dhcp_gives_address = false;   // DHCP liefert nichts ...
    r.configure(on_config());
    arm_healthy_modem(r.at);
    // ... und CGCONTRDP meldet keinen aktiven Kontext -> kein statischer
    // Fallback moeglich. "Es gibt ein usb0" ist dann keine Verbindung.
    r.at.set_reply("AT+CGCONTRDP=1", "+CGCONTRDP: 1,5,\"apn\",\"0.0.0.0\"\r\nOK\r\n");
    r.run(80);

    TCHECK(r.uplink.state() != LinkState::Connected);
    TCHECK(r.uplink.info().ipv4.empty());
    TCHECK(!r.uplink.has_internet());
}

void test_an_addressed_link_is_connected_and_reports_its_address()
{
    Rig r;
    r.be.iface_present = true;
    r.configure(on_config());
    arm_healthy_modem(r.at);
    r.run();

    TCHECK(r.uplink.state() == LinkState::Connected);
    const NetworkInfo n = r.uplink.info();
    // ECM wird jetzt IMMER per DHCP adressiert (auch im NIC-Modus) -- die reale
    // EC200A-Firmware serviert DHCP und liefert das richtige Gateway, waehrend
    // AT+CGCONTRDP einen DNS im Gateway-Feld zurueckgab (gemessen 2026-09-26).
    TCHECK(n.ipv4 == "192.168.43.100");
    TCHECK(n.gateway == "192.168.43.1");
    TCHECK(n.dhcp);
    TCHECK(r.uplink.has_internet());
}

void test_a_reachability_probe_overrides_the_optimistic_answer()
{
    // Ein Modem mit Adresse und Gateway, hinter dem nichts ist -- eine SIM
    // ohne Guthaben sieht genau so aus. Ohne Sonde waere das ein Uplink, auf
    // den das Failover gerne umschaltet.
    Rig r;
    r.be.iface_present = true;
    r.configure(on_config());
    arm_healthy_modem(r.at);
    r.run();
    TCHECK(r.uplink.has_internet());

    bool reachable = false;
    std::string asked;
    r.uplink.set_reachability([&](const std::string& ifname) { asked = ifname; return reachable; });
    TCHECK(!r.uplink.has_internet());
    TCHECK(asked == "usb0");                 // nach DEM Interface gefragt, nicht nach einem geratenen
    reachable = true;
    TCHECK(r.uplink.has_internet());
}

void test_signal_prefers_rsrp_over_csq()
{
    Rig r;
    r.be.iface_present = true;
    r.configure(on_config());
    arm_healthy_modem(r.at);
    r.at.set_reply("AT+QENG=\"servingcell\"",
                   "+QENG: \"servingcell\",\"NOCONN\",\"LTE\",\"FDD\",262,01,1A2B3C4,"
                   "201,1300,3,5,5,4E2F,-95,-11,-65,12,0\r\nOK\r\n");
    r.run();
    const UplinkMetrics m = r.uplink.metrics();
    // CSQ 22 ergaebe -69 dBm. RSRP ist die Messung, die bei LTE etwas bedeutet.
    TCHECK(m.rssi_dbm == -95);
    TCHECK(m.carrier);
    // Eine Kategorie ist keine Messung der Strecke.
    TCHECK(m.link_mbit == 0);
}

// ------------------------------------------------------------ Lebenszyklus

void test_a_health_probe_never_restarts_the_modem()
{
    // Die Regel aus AP-M5 Abschnitt 12: ein fehlgeschlagener Internettest darf
    // KEIN QNETDEVCTL, kein CFUN=1,1 und keinen ECM-Neuaufbau ausloesen. Sonst
    // bauen sich Connectivity-Pruefung und Modem-Lebenszyklus gegenseitig
    // Retry-Stuerme.
    Rig r;
    r.be.iface_present = true;
    r.configure(on_config());
    arm_healthy_modem(r.at);
    r.run();
    TCHECK(r.uplink.state() == LinkState::Connected);

    r.uplink.set_reachability([](const std::string&) { return false; });
    const size_t before = r.at.sent().size();
    for (int i = 0; i < 50; ++i) {
        (void)r.uplink.has_internet();
        (void)r.uplink.state();
        (void)r.uplink.info();
        (void)r.uplink.metrics();
    }
    TCHECK(r.at.sent().size() == before);   // kein einziges AT-Kommando
    TCHECK(r.be.teardowns == 0);
}

void test_the_connectivity_manager_never_drives_the_modem()
{
    // Derselbe Gedanke eine Ebene hoeher: der Manager waehlt aus, er schaltet
    // nicht. Ein evaluate(), das connect() ruft, wuerde ein Modem, das gerade
    // im Backoff sitzt, im Sekundentakt anstossen.
    Rig r;
    r.be.iface_present = true;
    r.configure(on_config());
    arm_healthy_modem(r.at);
    r.run();

    ConnectivityManager m;
    m.add(&r.uplink);
    const size_t before = r.at.sent().size();
    for (int i = 0; i < 20; ++i) m.evaluate();
    TCHECK(r.at.sent().size() == before);
    TCHECK(m.active_id() == "cellular");
}

void test_turning_it_off_tears_the_link_down()
{
    Rig r;
    r.be.iface_present = true;
    r.configure(on_config());
    arm_healthy_modem(r.at);
    r.run();
    TCHECK(r.uplink.state() == LinkState::Connected);

    cellular::CellularConfig off = on_config();
    off.enabled = false;
    r.configure(off);
    r.uplink.tick();

    TCHECK(r.uplink.state() == LinkState::Absent);
    TCHECK(r.be.teardowns == 1);
    TCHECK(r.uplink.info().ipv4.empty());
}

void test_off_and_on_again_reconnects_without_a_restart()
{
    // Ohne diese Pruefung waere "aus, speichern, wieder an" wirkungslos, bis
    // jemand den Daemon neu startet -- und ein Neustart ist auf dieser Kamera
    // der dokumentierte Hardlock-Ausloeser, also keine Antwort.
    Rig r;
    r.be.iface_present = true;
    r.configure(on_config());
    arm_healthy_modem(r.at);
    r.run();
    TCHECK(r.uplink.state() == LinkState::Connected);

    cellular::CellularConfig off = on_config(); off.enabled = false;
    r.configure(off);
    r.uplink.tick();
    TCHECK(r.uplink.state() == LinkState::Absent);

    r.configure(on_config());
    r.run();
    TCHECK(r.uplink.state() == LinkState::Connected);
}

void test_an_explicit_disconnect_is_not_undone_by_the_next_tick()
{
    // auto_connect heisst "von selbst verbinden", nicht "jeden Trennwunsch
    // innerhalb einer Sekunde ueberstimmen". Sonst waere der Aus-Knopf eine
    // Luege.
    Rig r;
    r.be.iface_present = true;
    r.configure(on_config());
    arm_healthy_modem(r.at);
    r.run();
    TCHECK(r.uplink.state() == LinkState::Connected);

    TCHECK(r.uplink.disconnect().is_ok());
    for (int i = 0; i < 5; ++i) { r.uplink.tick(); r.clock.t += 1000; }
    TCHECK(r.uplink.state() != LinkState::Connected);
    TCHECK(r.be.teardowns == 1);
}

void test_connect_is_refused_while_cellular_is_switched_off()
{
    Rig r;
    cellular::CellularConfig c = on_config(); c.enabled = false;
    r.configure(c);
    TCHECK(!r.uplink.connect().is_ok());
    TCHECK(r.at.sent().empty());
}

void test_no_retry_storm_while_the_modem_keeps_failing()
{
    // Der Backoff sitzt in EcmLink; hier wird geprueft, dass der Uplink ihn
    // nicht umgeht, indem er in jedem Takt eine neue Runde ausloest.
    Rig r;
    r.configure(on_config());
    arm_healthy_modem(r.at);
    r.at.set_reply("AT+QCFG=\"usbnet\"", "ERROR\r\n");

    // Gezaehlt wird das Kommando, mit dem eine ECM-Runde ANFAENGT, nicht der
    // gesamte AT-Verkehr. Die Statusabfrage laeuft in jedem Takt und soll das
    // auch -- sie ist der Grund, warum die Anzeige lebt. Der Punkt ist, dass
    // der Aufbauversuch NICHT in jedem Takt neu beginnt.
    for (int i = 0; i < 100; ++i) { r.uplink.tick(); r.clock.t += 100; }
    const int rounds = r.at.count_sent("AT+QCFG=\"usbnet\"");

    // Zehn Sekunden bei einem Backoff von 2/4/8/16/30 s sind eine Handvoll
    // Runden. Hundert Takte duerfen nicht hundert Runden bedeuten.
    TCHECK(rounds > 0);
    TCHECK(rounds < 10);
}

// ------------------------------------------- Zusammenspiel mit dem Manager

void test_cellular_takes_over_when_ethernet_disappears()
{
    struct FakeUplink : INetworkUplink {
        UplinkType t; LinkState st = LinkState::Connected; bool inet = true; NetworkInfo ni;
        std::string uid;
        FakeUplink(UplinkType type, const char* n) : t(type), uid(n) { ni.ifname = n; }
        std::string id() const override { return uid; }
        UplinkType type() const override { return t; }
        LinkState state() const override { return st; }
        NetworkInfo info() const override { return ni; }
        UplinkMetrics metrics() const override { return UplinkMetrics{}; }
        Result connect() override { return Result::ok(); }
        Result disconnect() override { return Result::ok(); }
        bool has_internet() const override { return inet; }
    };

    Rig r;
    r.be.iface_present = true;
    r.configure(on_config());
    arm_healthy_modem(r.at);
    r.run();

    FakeUplink eth(UplinkType::Ethernet, "eth0");
    ConnectivityManager m;
    m.add(&eth); m.add(&r.uplink);
    m.evaluate();
    TCHECK(m.active_id() == "eth0");

    eth.st = LinkState::Down; eth.inet = false;
    TCHECK(m.evaluate());
    TCHECK(m.active_id() == "cellular");

    // Und wieder zurueck, sobald das Kabel steckt.
    eth.st = LinkState::Connected; eth.inet = true;
    TCHECK(m.evaluate());
    TCHECK(m.active_id() == "eth0");
}

} // namespace

void run_cellular_uplink_tests()
{
    test_the_id_is_cellular_and_not_the_interface_name();
    test_the_id_survives_an_interface_rename();
    test_a_second_modem_can_carry_its_own_id();

    test_no_modem_is_absent_not_failed();
    test_switched_off_is_absent_and_sends_nothing();
    test_enabled_is_the_switch_even_with_auto_connect_off();
    test_a_modem_that_enumerates_but_does_not_answer_is_down();
    test_registered_but_no_datalink_is_not_connected();
    test_an_interface_without_an_address_is_not_connected();
    test_an_addressed_link_is_connected_and_reports_its_address();
    test_a_reachability_probe_overrides_the_optimistic_answer();
    test_signal_prefers_rsrp_over_csq();

    test_a_health_probe_never_restarts_the_modem();
    test_the_connectivity_manager_never_drives_the_modem();
    test_turning_it_off_tears_the_link_down();
    test_off_and_on_again_reconnects_without_a_restart();
    test_an_explicit_disconnect_is_not_undone_by_the_next_tick();
    test_connect_is_refused_while_cellular_is_switched_off();
    test_no_retry_storm_while_the_modem_keeps_failing();

    test_cellular_takes_over_when_ethernet_disappears();
}
