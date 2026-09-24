// Der ECM-Datenpfad als Zustandsmaschine, gegen Attrappen.
//
// Die Faelle hier sind die, an denen so eine Maschine in der Praxis scheitert:
// das Modem startet mitten im Ablauf neu, das Interface kommt spaet oder nie,
// DHCP antwortet nicht, der Link faellt nach Stunden. Am Geraet bekaeme man
// davon einen pro Nachmittag zu sehen.
#include "core/cellular/ecm_link.hpp"
#include "core/cellular/modem_ports.hpp"
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

// Ein Linux, das tut, was der Test sagt -- und mitschreibt, was verlangt wurde.
class FakeEcmBackend : public IEcmBackend {
public:
    bool        iface_present = false;
    std::string iface_name = "usb0";
    bool        dhcp_gives_address = true;
    bool        set_address_fails = false;

    int dhcp_starts = 0, dhcp_stops = 0, teardowns = 0, set_down_calls = 0;
    std::vector<std::string> dhcp_started_on;
    LinkAddress assigned;

    bool find_interface(EcmInterface& out) override
    {
        if (!iface_present) return false;
        out.name = iface_name; out.up = up_; out.carrier = true;
        return true;
    }
    bool set_up(const std::string&, bool up) override { if (!up) ++set_down_calls; up_ = up; return true; }
    bool dhcp_start(const std::string& ifname) override
    {
        ++dhcp_starts; dhcp_started_on.push_back(ifname);
        running_ = true;
        return true;
    }
    bool dhcp_stop(const std::string&) override { ++dhcp_stops; running_ = false; return true; }
    bool read_address(const std::string&, LinkAddress& out) override
    {
        if (assigned.has_address()) { out = assigned; return true; }
        if (!running_ || !dhcp_gives_address) return false;
        out.ipv4 = "192.168.43.100";
        out.gateway = "192.168.43.1";
        out.dns1 = "192.168.43.1";
        return true;
    }
    bool set_address(const std::string&, const LinkAddress& a) override
    {
        if (set_address_fails) return false;
        assigned = a;
        return true;
    }
    void teardown(const std::string&) override { ++teardowns; assigned = LinkAddress{}; }

    bool dhcp_running() const { return running_; }

private:
    bool up_ = false;
    bool running_ = false;
};

// Eine Uhr, die der Test stellt.
struct Clock {
    uint64_t t = 1000;
    uint64_t operator()() const { return t; }
};

CellularStatus healthy_status()
{
    CellularStatus s;
    s.present = true;
    s.responsive = true;
    s.sim = SimState::Ready;
    s.registration = RegState::RegisteredHome;
    return s;
}

void arm_ecm_ready(ScriptedAtTransport& t)
{
    t.reply("AT+QCFG=\"usbnet\"", "+QCFG: \"usbnet\",1\r\nOK\r\n");
    t.reply("AT+QCFG=\"nat\"", "+QCFG: \"nat\",1\r\nOK\r\n");
    t.reply("AT+CGDCONT=1,\"IP\",\"internet.t-d1.de\"", "OK\r\n");
    t.reply("AT+QICSGP=1,1,\"internet.t-d1.de\",\"\",\"\",0", "OK\r\n");
    t.reply("AT+QNETDEVCTL=3,1", "OK\r\n");
    t.reply("AT+CGCONTRDP=1",
            "+CGCONTRDP: 1,5,\"internet.t-d1.de\",\"37.81.97.187.255.255.255.240\","
            "\"37.81.97.185\",\"10.74.210.210\",\"10.74.210.211\"\r\nOK\r\n");
}

CellularConfig telekom_config()
{
    CellularConfig c;
    c.enabled = true;
    c.apn = "internet.t-d1.de";
    c.pdp = PdpType::Ipv4;
    c.auth = AuthMode::None;
    c.nic_mode = true;
    return c;
}

// Bis zu n Ticks, oder bis der Zustand erreicht ist.
DataLinkState run(EcmLink& l, const CellularStatus& s, Clock& c, int n = 12, uint64_t step = 500)
{
    for (int i = 0; i < n; ++i) {
        const DataLinkState st = l.tick(s).state;
        if (st == DataLinkState::Up || st == DataLinkState::Failed) return st;
        c.t += step;
    }
    return l.state().state;
}

// ------------------------------------------------------------ Interface ----

void test_the_ecm_interface_is_found_by_device_not_by_name()
{
    // usb0 kann alles Moegliche sein. Auf dieser Kamera haengt womoeglich noch
    // ein WLAN-Adapter am Bus, und der bekommt auch einen Namen.
    const std::vector<NetDeviceInfo> devs = {
        {"eth0",  "stmmaceth",  "",     ""},
        {"usb0",  "aic8800",    "a69c", "88dc"},
        {"eth1",  "cdc_ether",  "2c7c", "6005"},
    };
    TCHECK(find_ecm_interface(devs) == "eth1");
}

void test_an_rndis_interface_is_not_an_ecm_interface()
{
    // Der Unterschied entscheidet: bindet rndis_host, steht das Modem noch auf
    // usbnet=3. Das als ECM zu melden hiesse, der Zustandsmaschine zu sagen,
    // der Moduswechsel sei erledigt -- und sie wuerde nie umstellen.
    const std::vector<NetDeviceInfo> devs = {
        {"usb0", "rndis_host", "2c7c", "6005"},
    };
    TCHECK(find_ecm_interface(devs).empty());

    const std::vector<NetDeviceInfo> ncm = {{"wwan0", "cdc_ncm", "2c7c", "6005"}};
    TCHECK(find_ecm_interface(ncm) == "wwan0");
}

// --------------------------------------------------------------- CGCONTRDP --

void test_cgcontrdp_separates_address_from_netmask()
{
    // 3GPP packt Adresse UND Maske in eine Punktliste mit acht Oktetten. Wer
    // das als IP liest, konfiguriert 37.81.97.187.255.255.255.240 aufs
    // Interface.
    const PdpContextParams p = parse_cgcontrdp(
        "+CGCONTRDP: 1,5,\"internet.t-d1.de\",\"37.81.97.187.255.255.255.240\","
        "\"37.81.97.185\",\"10.74.210.210\",\"10.74.210.211\"\r\nOK\r\n");
    TCHECK(p.apn == "internet.t-d1.de");
    TCHECK(p.ipv4 == "37.81.97.187");
    TCHECK(p.netmask == "255.255.255.240");
    TCHECK(p.gateway == "37.81.97.185");
    TCHECK(p.dns1 == "10.74.210.210");
    TCHECK(p.dns2 == "10.74.210.211");

    // Vier Oktette: nur Adresse, keine Maske.
    const PdpContextParams four = parse_cgcontrdp(
        "+CGCONTRDP: 1,5,\"apn\",\"10.1.2.3\",\"10.1.2.1\"\r\nOK\r\n");
    TCHECK(four.ipv4 == "10.1.2.3" && four.netmask.empty());

    // Nicht aktiver Kontext.
    TCHECK(parse_cgcontrdp("+CGCONTRDP: 1,5,\"apn\",\"0.0.0.0\"\r\nOK\r\n").ipv4.empty());
    TCHECK(parse_cgcontrdp("ERROR\r\n").ipv4.empty());
}

void test_qcfg_value_is_absent_when_unreadable()
{
    TCHECK(parse_qcfg_int("+QCFG: \"usbnet\",1\r\nOK\r\n", "usbnet").value_or(-1) == 1);
    TCHECK(parse_qcfg_int("+QCFG: \"nat\",0\r\nOK\r\n", "nat").value_or(-1) == 0);
    // 0 ist ein gueltiger Modus -- "nicht lesbar" darf nicht als 0 durchgehen.
    TCHECK(!parse_qcfg_int("ERROR\r\n", "nat").has);
    TCHECK(!parse_qcfg_int("+QCFG: \"usbnet\",1\r\nOK\r\n", "nat").has);
    TCHECK(!parse_qcfg_int("", "nat").has);
}

// ------------------------------------------------------- Zustandsmaschine ---

void test_a_modem_already_in_ecm_mode_comes_up_without_a_reboot()
{
    ScriptedAtTransport t; arm_ecm_ready(t);
    FakeEcmBackend be; be.iface_present = true; be.iface_name = "usb0";
    Clock c;
    EcmLink l(t, be);
    l.set_clock([&c] { return c.t; });
    l.set_config(telekom_config());
    l.connect();

    TCHECK(run(l, healthy_status(), c) == DataLinkState::Up);
    TCHECK(l.interface_name() == "usb0");
    // NIC-Modus: die Adresse kommt vom Modem, NICHT per DHCP.
    TCHECK(l.address().ipv4 == "37.81.97.187");
    TCHECK(l.address().gateway == "37.81.97.185");
    TCHECK(be.dhcp_starts == 0);
    TCHECK(t.count_sent("AT+CFUN") == 0);      // kein Neustart noetig
}

void test_the_verified_qnetdevctl_variant_is_used()
{
    // Die Linux-Kurzanleitung im Referenzrepo sagt 1,1,1. Der Code, der am
    // Geraet lief, nimmt 3,1 -- Typ 3 ist auto-connect und persistent, Typ 1
    // ist "once" und faellt bei jedem Modem-Neustart zurueck.
    ScriptedAtTransport t; arm_ecm_ready(t);
    FakeEcmBackend be; be.iface_present = true;
    Clock c;
    EcmLink l(t, be);
    l.set_clock([&c] { return c.t; });
    l.set_config(telekom_config());
    l.connect();
    run(l, healthy_status(), c);
    TCHECK(t.count_sent("AT+QNETDEVCTL=3,1") == 1);
    TCHECK(t.count_sent("AT+QNETDEVCTL=1,1,1") == 0);
}

void test_a_mode_switch_reboots_once_and_waits_for_the_modem()
{
    ScriptedAtTransport t;
    t.reply("AT+QCFG=\"usbnet\"", "+QCFG: \"usbnet\",3\r\nOK\r\n");   // RNDIS
    t.reply("AT+QCFG=\"usbnet\",1", "OK\r\n");
    t.reply("AT+CFUN=1,1", "OK\r\n");
    FakeEcmBackend be;
    Clock c;
    EcmLink l(t, be);
    l.set_clock([&c] { return c.t; });
    l.set_config(telekom_config());
    l.connect();

    TCHECK(l.tick(healthy_status()).state == DataLinkState::WaitReenumeration);
    TCHECK(t.count_sent("AT+CFUN=1,1") == 1);

    // Das Modem verschwindet -- erwartet, kein Fehler.
    CellularStatus gone;
    gone.present = false;
    for (int i = 0; i < 20; ++i) { c.t += 1000; l.tick(gone); }
    TCHECK(l.state().state == DataLinkState::WaitReenumeration);
    TCHECK(t.count_sent("AT+CFUN=1,1") == 1);     // NICHT noch einmal

    // Es kommt zurueck, jetzt als ECM. set_reply, nicht reply: die alte
    // Antwort "3" darf nicht mehr in der Warteschlange stehen, sonst sieht die
    // Maschine nach dem Neustart erneut RNDIS und gibt auf -- richtig
    // gehandelt, aber die Lage gaebe es nur im Test.
    t.set_reply("AT+QCFG=\"usbnet\"", "+QCFG: \"usbnet\",1\r\nOK\r\n");
    t.reply("AT+QCFG=\"nat\"", "+QCFG: \"nat\",1\r\nOK\r\n");
    t.reply("AT+CGDCONT=1,\"IP\",\"internet.t-d1.de\"", "OK\r\n");
    t.reply("AT+QICSGP=1,1,\"internet.t-d1.de\",\"\",\"\",0", "OK\r\n");
    t.reply("AT+QNETDEVCTL=3,1", "OK\r\n");
    t.reply("AT+CGCONTRDP=1",
            "+CGCONTRDP: 1,5,\"internet.t-d1.de\",\"37.81.97.187.255.255.255.240\","
            "\"37.81.97.185\",\"10.74.210.210\",\"10.74.210.211\"\r\nOK\r\n");
    be.iface_present = true;
    TCHECK(run(l, healthy_status(), c) == DataLinkState::Up);
    TCHECK(t.count_sent("AT+CFUN=1,1") == 1);
}

void test_a_modem_that_cannot_do_ecm_does_not_reboot_forever()
{
    // Das ist der Grund fuer die Einmalsperre. Ohne sie: umstellen, rebooten,
    // pruefen, immer noch RNDIS, umstellen, rebooten -- fuer immer.
    ScriptedAtTransport t;
    t.reply("AT+QCFG=\"usbnet\"", "+QCFG: \"usbnet\",3\r\nOK\r\n");   // bleibt RNDIS
    t.reply("AT+QCFG=\"usbnet\",1", "OK\r\n");
    t.reply("AT+CFUN=1,1", "OK\r\n");
    FakeEcmBackend be;
    Clock c;
    EcmLink l(t, be);
    l.set_clock([&c] { return c.t; });
    l.set_config(telekom_config());
    l.connect();

    for (int i = 0; i < 200; ++i) { l.tick(healthy_status()); c.t += 5000; }
    TCHECK(t.count_sent("AT+CFUN=1,1") == 1);
    TCHECK(l.state().state == DataLinkState::Failed);
    TCHECK(l.state().detail.find("nicht unterstuetzt") != std::string::npos);
}

void test_an_unreadable_mode_changes_nothing()
{
    // Wenn nicht feststeht, worauf das Modem steht, wird nichts umgestellt.
    // Eine persistente Aenderung auf Verdacht ist schlimmer als keine.
    ScriptedAtTransport t;
    t.reply("AT+QCFG=\"usbnet\"", "ERROR\r\n");
    FakeEcmBackend be;
    Clock c;
    EcmLink l(t, be);
    l.set_clock([&c] { return c.t; });
    l.set_config(telekom_config());
    l.connect();
    l.tick(healthy_status());
    TCHECK(l.state().state == DataLinkState::Failed);
    TCHECK(t.count_sent("AT+QCFG=\"usbnet\",") == 0);
    TCHECK(t.count_sent("AT+CFUN") == 0);
}

void test_nothing_happens_without_sim_or_registration()
{
    ScriptedAtTransport t; arm_ecm_ready(t);
    FakeEcmBackend be; be.iface_present = true;
    Clock c;
    EcmLink l(t, be);
    l.set_clock([&c] { return c.t; });
    l.set_config(telekom_config());
    l.connect();

    CellularStatus s = healthy_status();
    s.sim = SimState::PinRequired;
    s.sim_detail = "SIM verlangt eine PIN";
    TCHECK(l.tick(s).state == DataLinkState::WaitSim);
    TCHECK(t.sent().empty());                    // kein einziges Kommando

    s.sim = SimState::Ready;
    s.registration = RegState::Searching;
    TCHECK(l.tick(s).state == DataLinkState::WaitRegistration);
    TCHECK(t.sent().empty());
    TCHECK(l.state().detail.find("searching") != std::string::npos);
}

void test_dhcp_in_routing_mode_and_only_on_our_interface()
{
    ScriptedAtTransport t; arm_ecm_ready(t);
    t.reply("AT+QCFG=\"nat\"", "+QCFG: \"nat\",0\r\nOK\r\n");   // Routing
    FakeEcmBackend be; be.iface_present = true; be.iface_name = "usb0";
    Clock c;
    EcmLink l(t, be);
    l.set_clock([&c] { return c.t; });
    CellularConfig cfg = telekom_config();
    cfg.nic_mode = false;
    l.set_config(cfg);
    l.connect();

    TCHECK(run(l, healthy_status(), c) == DataLinkState::Up);
    TCHECK(be.dhcp_starts == 1);
    TCHECK(be.dhcp_started_on.size() == 1 && be.dhcp_started_on[0] == "usb0");
    TCHECK(l.address().ipv4 == "192.168.43.100");
    // Die oeffentliche Adresse hinter dem Modem-NAT wird trotzdem gemerkt --
    // das ist die, die von aussen zaehlt.
    TCHECK(l.state().modem_pdp_address == "37.81.97.187");
    TCHECK(l.address().ipv4 != l.state().modem_pdp_address);
}

void test_dhcp_is_not_started_twice()
{
    ScriptedAtTransport t; arm_ecm_ready(t);
    t.reply("AT+QCFG=\"nat\"", "+QCFG: \"nat\",0\r\nOK\r\n");
    FakeEcmBackend be; be.iface_present = true; be.dhcp_gives_address = false;
    Clock c;
    EcmLink l(t, be);
    l.set_clock([&c] { return c.t; });
    CellularConfig cfg = telekom_config(); cfg.nic_mode = false;
    l.set_config(cfg);
    l.connect();

    for (int i = 0; i < 10; ++i) { l.tick(healthy_status()); c.t += 1000; }
    TCHECK(be.dhcp_starts == 1);       // nicht einmal je Tick
}

void test_dhcp_timeout_fails_and_stops_its_own_client()
{
    ScriptedAtTransport t; arm_ecm_ready(t);
    t.reply("AT+QCFG=\"nat\"", "+QCFG: \"nat\",0\r\nOK\r\n");
    FakeEcmBackend be; be.iface_present = true; be.dhcp_gives_address = false;
    Clock c;
    EcmLink l(t, be);
    l.set_clock([&c] { return c.t; });
    CellularConfig cfg = telekom_config(); cfg.nic_mode = false;
    l.set_config(cfg);
    l.connect();

    // Bis zum ersten Fehlschlag laufen lassen -- danach versucht die Maschine
    // es absichtlich wieder, also waere "DHCP laeuft nicht" am Ende einer
    // langen Schleife die falsche Frage.
    for (int i = 0; i < 60 && l.state().state != DataLinkState::Failed; ++i) {
        l.tick(healthy_status());
        c.t += 2000;
    }
    TCHECK(l.state().state == DataLinkState::Failed);
    TCHECK(l.state().detail.find("DHCP") != std::string::npos);
    TCHECK(be.dhcp_stops == 1);          // der eigene Client, genau einmal
    TCHECK(!be.dhcp_running());
}

void test_a_late_interface_is_waited_for_a_missing_one_is_not_forever()
{
    ScriptedAtTransport t; arm_ecm_ready(t);
    FakeEcmBackend be; be.iface_present = false;
    Clock c;
    EcmLink l(t, be);
    l.set_clock([&c] { return c.t; });
    l.set_config(telekom_config());
    l.connect();

    // Kommt spaet: das ist in Ordnung.
    for (int i = 0; i < 5; ++i) { l.tick(healthy_status()); c.t += 1000; }
    TCHECK(l.state().state == DataLinkState::WaitNetif);
    be.iface_present = true;
    TCHECK(run(l, healthy_status(), c) == DataLinkState::Up);
}

void test_an_interface_that_never_appears_gives_a_reason()
{
    ScriptedAtTransport t; arm_ecm_ready(t);
    FakeEcmBackend be; be.iface_present = false;
    Clock c;
    EcmLink l(t, be);
    l.set_clock([&c] { return c.t; });
    l.set_config(telekom_config());
    l.connect();
    for (int i = 0; i < 40; ++i) { l.tick(healthy_status()); c.t += 2000; }
    TCHECK(l.state().state == DataLinkState::Failed);
    TCHECK(l.state().detail.find("cdc_ether") != std::string::npos);
}

void test_link_loss_after_up_returns_to_addressing_not_to_zero()
{
    ScriptedAtTransport t; arm_ecm_ready(t);
    t.reply("AT+QCFG=\"nat\"", "+QCFG: \"nat\",0\r\nOK\r\n");
    FakeEcmBackend be; be.iface_present = true;
    Clock c;
    EcmLink l(t, be);
    l.set_clock([&c] { return c.t; });
    CellularConfig cfg = telekom_config(); cfg.nic_mode = false;
    l.set_config(cfg);
    l.connect();
    TCHECK(run(l, healthy_status(), c) == DataLinkState::Up);

    // Lease weg, Interface noch da.
    be.dhcp_gives_address = false;
    c.t += 1000;
    TCHECK(l.tick(healthy_status()).state == DataLinkState::Addressing);

    // Interface weg: das ist mehr als ein verlorener Lease.
    be.iface_present = false;
    c.t += 1000;
    l.tick(healthy_status());
    TCHECK(l.state().state == DataLinkState::Failed);
    TCHECK(l.state().detail.find("verschwunden") != std::string::npos);
}

void test_failures_back_off_instead_of_hammering()
{
    TCHECK(EcmLink::backoff_ms(0) == 0);
    TCHECK(EcmLink::backoff_ms(1) == 2000);
    TCHECK(EcmLink::backoff_ms(2) == 4000);
    TCHECK(EcmLink::backoff_ms(3) == 8000);
    TCHECK(EcmLink::backoff_ms(10) == 30000);
    TCHECK(EcmLink::backoff_ms(1000) == 30000);   // gedeckelt, nie unendlich

    // Und die Maschine haelt sich daran: nach einem Fehlschlag wird bis zum
    // Ablauf der Wartezeit KEIN Kommando geschickt.
    ScriptedAtTransport t;
    t.reply("AT+QCFG=\"usbnet\"", "ERROR\r\n");
    FakeEcmBackend be;
    Clock c;
    EcmLink l(t, be);
    l.set_clock([&c] { return c.t; });
    l.set_config(telekom_config());
    l.connect();
    l.tick(healthy_status());
    const size_t after_first = t.sent().size();
    for (int i = 0; i < 5; ++i) { c.t += 100; l.tick(healthy_status()); }
    TCHECK(t.sent().size() == after_first);       // nichts in der Sperrzeit
}

void test_disconnect_cleans_up_and_is_idempotent()
{
    ScriptedAtTransport t; arm_ecm_ready(t);
    t.reply("AT+QCFG=\"nat\"", "+QCFG: \"nat\",0\r\nOK\r\n");
    FakeEcmBackend be; be.iface_present = true;
    Clock c;
    EcmLink l(t, be);
    l.set_clock([&c] { return c.t; });
    CellularConfig cfg = telekom_config(); cfg.nic_mode = false;
    l.set_config(cfg);
    l.connect();
    run(l, healthy_status(), c);

    l.disconnect();
    TCHECK(l.state().state == DataLinkState::Disabled);
    TCHECK(!l.is_up());
    TCHECK(be.dhcp_stops == 1);
    TCHECK(be.teardowns == 1);
    TCHECK(l.address().ipv4.empty());

    // GENAU EIN Wunsch an den Helfer, nicht zwei.
    //
    // Der Weg dorthin ist eine einzelne Zeile in einer Datei, die der Helfer
    // im Sekundentakt liest. Wer teardown() und danach set_up(false) ruft,
    // ueberschreibt das "stop" mit einem "down", bevor es jemand gesehen hat --
    // der DHCP-Client liefe weiter und holte sich beim naechsten Lease die
    // Adresse zurueck, die hier gerade abgeraeumt wurde. Der Helfer nimmt das
    // Interface im stop selbst herunter; hier darf nichts mehr nachkommen.
    TCHECK(be.set_down_calls == 0);

    l.disconnect();                 // zweimal ist kein Fehler
    l.disconnect();
    TCHECK(be.dhcp_stops == 1);     // aber auch keine weiteren Aktionen
    TCHECK(be.teardowns == 1);

    // Und ein Tick danach tut nichts.
    const size_t sent = t.sent().size();
    l.tick(healthy_status());
    TCHECK(t.sent().size() == sent);
}

void test_repeated_connect_is_a_noop()
{
    ScriptedAtTransport t; arm_ecm_ready(t);
    FakeEcmBackend be; be.iface_present = true;
    Clock c;
    EcmLink l(t, be);
    l.set_clock([&c] { return c.t; });
    l.set_config(telekom_config());
    l.connect();
    run(l, healthy_status(), c);
    const size_t sent = t.sent().size();
    l.connect(); l.connect();
    TCHECK(l.is_up());
    TCHECK(t.sent().size() == sent);
}

void test_an_apn_is_required_before_anything_is_configured()
{
    ScriptedAtTransport t; arm_ecm_ready(t);
    FakeEcmBackend be; be.iface_present = true;
    Clock c;
    EcmLink l(t, be);
    l.set_clock([&c] { return c.t; });
    CellularConfig cfg = telekom_config(); cfg.apn.clear();
    l.set_config(cfg);
    l.connect();
    l.tick(healthy_status());
    TCHECK(l.state().state == DataLinkState::Failed);
    TCHECK(l.state().detail.find("APN") != std::string::npos);
    TCHECK(t.count_sent("AT+CGDCONT") == 0);
}

void test_a_rejected_apn_is_reported_not_retried_immediately()
{
    // NICHT arm_ecm_ready: das registriert CGDCONT bereits mit OK, und die
    // Fehlerantwort haette dahinter in der Warteschlange gestanden. Der Test
    // haette dann den Erfolgsfall geprueft und sich gruen gemeldet -- beim
    // ersten Lauf genau so passiert.
    ScriptedAtTransport t;
    t.reply("AT+QCFG=\"usbnet\"", "+QCFG: \"usbnet\",1\r\nOK\r\n");
    t.reply("AT+QCFG=\"nat\"", "+QCFG: \"nat\",1\r\nOK\r\n");
    t.reply("AT+CGDCONT=1,\"IP\",\"internet.t-d1.de\"", "+CME ERROR: 50\r\n");
    FakeEcmBackend be; be.iface_present = true;
    Clock c;
    EcmLink l(t, be);
    l.set_clock([&c] { return c.t; });
    l.set_config(telekom_config());
    l.connect();
    l.tick(healthy_status());
    TCHECK(l.state().state == DataLinkState::Failed);
    TCHECK(l.state().detail.find("CGDCONT") != std::string::npos);
    TCHECK(t.count_sent("AT+QNETDEVCTL") == 0);     // nicht weitergemacht
}

void test_the_link_never_falls_back_to_ppp()
{
    // Wer ECM konfiguriert hat, bekommt ECM oder einen Fehler. Ein heimlicher
    // Wechsel waere genau die Ueberraschung, die das WAN-Modell verhindert.
    ScriptedAtTransport t;
    t.reply("AT+QCFG=\"usbnet\"", "ERROR\r\n");
    FakeEcmBackend be;
    Clock c;
    EcmLink l(t, be);
    l.set_clock([&c] { return c.t; });
    l.set_config(telekom_config());
    l.connect();
    for (int i = 0; i < 100; ++i) { l.tick(healthy_status()); c.t += 5000; }
    for (const std::string& s : t.sent()) {
        TCHECK(s.compare(0, 3, "ATD") != 0);
        TCHECK(s.find("QNETDEVCTL=0") == std::string::npos);
    }
}

} // namespace

void run_ecm_link_tests()
{
    test_the_ecm_interface_is_found_by_device_not_by_name();
    test_an_rndis_interface_is_not_an_ecm_interface();
    test_cgcontrdp_separates_address_from_netmask();
    test_qcfg_value_is_absent_when_unreadable();

    test_a_modem_already_in_ecm_mode_comes_up_without_a_reboot();
    test_the_verified_qnetdevctl_variant_is_used();
    test_a_mode_switch_reboots_once_and_waits_for_the_modem();
    test_a_modem_that_cannot_do_ecm_does_not_reboot_forever();
    test_an_unreadable_mode_changes_nothing();
    test_nothing_happens_without_sim_or_registration();

    test_dhcp_in_routing_mode_and_only_on_our_interface();
    test_dhcp_is_not_started_twice();
    test_dhcp_timeout_fails_and_stops_its_own_client();
    test_a_late_interface_is_waited_for_a_missing_one_is_not_forever();
    test_an_interface_that_never_appears_gives_a_reason();
    test_link_loss_after_up_returns_to_addressing_not_to_zero();

    test_failures_back_off_instead_of_hammering();
    test_disconnect_cleans_up_and_is_idempotent();
    test_repeated_connect_is_a_noop();
    test_an_apn_is_required_before_anything_is_configured();
    test_a_rejected_apn_is_reported_not_retried_immediately();
    test_the_link_never_falls_back_to_ppp();
}
