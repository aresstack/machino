// AP3 (Feature 2): IpsecConfig/IpsecService am Fake-Backend. Was hier
// zaehlt: die Ablehnung MIT NAMEN (kein stilles Downgrade), der write-only-
// PSK (nie zurueck, wird beim Speichern ohne neuen Wert weitergetragen),
// atomare Persistenz beider Dateien, und das Status-Mapping 1:1 aus den
// weirdikectl-Zeilen inklusive der getrennten Fehlerklassen 24/14/38/0.
#include "core/net/ipsec_config.hpp"
#include "core/net/ipsec_service.hpp"

#include <cstdio>
#include <string>

using namespace machino::ipsec;

extern int g_fail_ext, g_pass_ext;
#define ICHECK(cond) do { if (cond) { ++g_pass_ext; } else { ++g_fail_ext; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

namespace {

const char* MCONF = "test_ipsec_machino.conf";
const char* DCONF = "test_ipsec_daemon.conf";

struct FakeBackend : IIpsecBackend {
    bool running = false;
    bool start_ok = true, stop_ok = true;
    int  starts = 0, stops = 0;
    std::string status_text;
    // AP5:
    bool resolve_ok = true;
    std::string resolved = "203.0.113.5";
    struct Route { std::string ip, dev, gw; };
    std::vector<Route> routes;                 // die aktuell installierten
    int route_adds = 0, route_dels = 0;

    bool daemon_running() override { return running; }
    bool start_daemon(std::string& err) override {
        ++starts;
        if (!start_ok) { err = "start kaputt"; return false; }
        running = true; return true;
    }
    bool stop_daemon(std::string& err) override {
        ++stops;
        if (!stop_ok) { err = "stop kaputt"; return false; }
        running = false; return true;
    }
    bool ctl_status(std::string& out) override { out = status_text; return running; }
    bool resolve4(const std::string&, std::string& out) override {
        if (!resolve_ok) return false;
        out = resolved; return true;
    }
    bool add_peer_route(const std::string& ip, const std::string& dev,
                        const std::string& gw, std::string&) override {
        ++route_adds; routes.push_back({ip, dev, gw}); return true;
    }
    bool del_peer_route(const std::string& ip, const std::string& dev,
                        const std::string& gw, std::string&) override {
        ++route_dels;
        for (size_t i = 0; i < routes.size(); ++i)
            if (routes[i].ip == ip && routes[i].dev == dev && routes[i].gw == gw) {
                routes.erase(routes.begin() + i);
                return true;
            }
        return true;                           // idempotent, wie das echte Backend
    }
    // AP12: der Test-Ping -- was der Service dem Backend gibt, und was zurueckkommt.
    PingRequest last_ping; int pings = 0; bool ping_ok = true; double rtt = 45.0;
    bool ping(const PingRequest& r, PingResult& o) override {
        ++pings; last_ping = r; o = PingResult{}; o.sent = r.count;
        if (!ping_ok) { o.error = "keine Antwort (" + std::to_string(r.timeout_ms) + " ms je Echo)"; return false; }
        o.received = r.count; o.rtt_min_ms = rtt - 1; o.rtt_avg_ms = rtt; o.rtt_max_ms = rtt + 1; return true;
    }
};

// AP5 §11: zwei Uplinks mit verschiedenen Adressen -- der Test sieht, WOHIN
// gebunden wurde.
struct FakeUplinks : IIpsecUplinks {
    UnderlayView eth, cell;
    FakeUplinks() {
        eth.usable = true;  eth.kind = "ethernet"; eth.ifname = "eth0";
        eth.ipv4 = "10.0.0.5"; eth.gateway = "10.0.0.1";
        cell.usable = true; cell.kind = "cellular"; cell.ifname = "usb0";
        cell.ipv4 = "100.71.3.9"; cell.gateway = "100.71.3.1";
    }
    bool select(Underlay w, UnderlayView& out, std::string& err) override {
        if (w == Underlay::Cellular) { out = cell; if (!cell.usable) err = "cellular nicht verbunden"; return cell.usable; }
        if (w == Underlay::Ethernet) { out = eth;  if (!eth.usable)  err = "ethernet nicht verbunden"; return eth.usable; }
        if (w == Underlay::Auto)     { out = eth.usable ? eth : cell; return out.usable; }
        err = "kein solcher Uplink"; return false;
    }
};

// AP9: set_config nimmt jetzt IpsecSecrets; die alten PSK-Tests reichen nur
// den PSK.
IpsecSecrets S(const std::string& psk) { IpsecSecrets s; s.psk = psk; return s; }

IpsecConfig sample()
{
    IpsecConfig c;
    c.enabled = true;
    c.gateway = "vpn.example.org";
    c.local_id = "cam.test";
    c.remote_id = "vpn.test";
    c.local_subnet = "10.77.0.2/32";
    c.remote_subnet = "10.66.0.0/24";
    return c;
}

std::string slurp(const char* p)
{
    FILE* f = fopen(p, "rb");
    if (!f) return {};
    std::string s; char b[512]; size_t n;
    while ((n = fread(b, 1, sizeof(b), f)) > 0) s.append(b, n);
    fclose(f);
    return s;
}

void test_config_roundtrip()
{
    const IpsecConfig c = sample();
    ICHECK(validate(c).empty());
    IpsecConfig back;
    std::string err;
    ICHECK(from_machino_conf(to_machino_conf(c), back, err));
    ICHECK(back.enabled == c.enabled);
    ICHECK(back.gateway == c.gateway);
    ICHECK(back.port == c.port);
    ICHECK(back.local_subnet == c.local_subnet);
    ICHECK(back.remote_subnet == c.remote_subnet);
    ICHECK(back.nat_t == c.nat_t);
}

void test_validate_names_the_problem()
{
    IpsecConfig c = sample();
    c.ike_enc = {"chacha20"};                          // AP11: im Katalog, in diesem Build nicht implementiert
    std::string e = validate(c);
    // Die Ablehnung nennt den Algorithmus BEIM NAMEN.
    ICHECK(e.find("chacha20") != std::string::npos);
    ICHECK(e.find("ikeEnc") != std::string::npos);

    c = sample(); c.esp_hash = {"sha256", "md5"};      // AP11: Liste erlaubt, aber md5 nicht implementiert
    ICHECK(!validate(c).empty());

    c = sample(); c.remote_subnet = "10.66.0.0/33";
    e = validate(c);
    ICHECK(e.find("remoteSubnet") != std::string::npos);

    c = sample(); c.gateway = "bad host!";
    ICHECK(validate(c).find("gateway") != std::string::npos);

    c = sample(); c.remote_subnet.clear();           // enabled braucht das Split-Netz
    ICHECK(!validate(c).empty());

    Underlay u;
    ICHECK(!underlay_from_name("modem", u));
    ICHECK(underlay_from_name("cellular", u) && u == Underlay::Cellular);
}

void test_psk_write_only_carry()
{
    // Erster Schreibvorgang traegt den PSK, der zweite (ohne PSK) MUSS ihn
    // unveraendert weitertragen -- und er darf nur in der Daemon-Datei stehen.
    bool present = false;
    const std::string d1 = to_weirdike_conf(sample(), S("s3cret-psk"), "", &present);
    ICHECK(present);
    ICHECK(d1.find("psk = s3cret-psk\n") != std::string::npos);

    const std::string d2 = to_weirdike_conf(sample(), S(""), d1, &present);
    ICHECK(present);
    ICHECK(d2.find("psk = s3cret-psk\n") != std::string::npos);

    const std::string d3 = to_weirdike_conf(sample(), S(""), "", &present);
    ICHECK(!present);
    ICHECK(d3.find("psk =") == std::string::npos);

    // Die machino-Datei kennt das Secret NICHT (die 'auth = psk'-Zeile ist
    // KEIN Secret; die verbotene Zeile ist 'psk = <wert>').
    ICHECK(to_machino_conf(sample()).find("psk =") == std::string::npos);
}

void test_service_persists_and_guards()
{
    remove(MCONF); remove(DCONF);
    FakeBackend be;
    IpsecService svc(be, MCONF, DCONF);

    // enabled=true ohne PSK: das Speichern wird abgelehnt, BEVOR etwas auf
    // der Platte landet.
    ICHECK(!svc.set_config(sample(), S("")).empty());
    ICHECK(slurp(MCONF).empty());

    ICHECK(svc.set_config(sample(), S("s3cret-psk")).empty());
    ICHECK(svc.psk_set());
    const std::string m = slurp(MCONF);
    ICHECK(!m.empty());
    ICHECK(m.find("s3cret-psk") == std::string::npos);      // Secret NIE in der machino-Datei
    ICHECK(slurp(DCONF).find("psk = s3cret-psk") != std::string::npos);

    // Reload liest denselben Stand.
    const IpsecConfig c2 = svc.config();
    ICHECK(c2.enabled && c2.gateway == "vpn.example.org");

    // Speichern ohne neuen PSK laesst den alten stehen.
    IpsecConfig c3 = c2; c3.port = 4500;
    ICHECK(svc.set_config(c3, S("")).empty());
    ICHECK(svc.psk_set());
    ICHECK(slurp(DCONF).find("psk = s3cret-psk") != std::string::npos);

    remove(MCONF); remove(DCONF);
}

void test_connect_disconnect()
{
    remove(MCONF); remove(DCONF);
    FakeBackend be;
    IpsecService svc(be, MCONF, DCONF);

    // disabled: connect verweigert, disconnect ist idempotent-ok.
    ICHECK(!svc.connect().empty());
    ICHECK(svc.disconnect().empty());
    ICHECK(be.starts == 0 && be.stops == 0);

    ICHECK(svc.set_config(sample(), S("s3cret-psk")).empty());
    ICHECK(svc.connect().empty());
    ICHECK(be.starts == 1 && be.running);
    ICHECK(svc.connect().empty());          // schon verbunden: kein 2. Start
    ICHECK(be.starts == 1);

    ICHECK(svc.disconnect().empty());
    ICHECK(be.stops == 1 && !be.running);

    // Backend-Fehler kommen als Text an, nicht als stilles Okay.
    be.start_ok = false;
    ICHECK(svc.connect() == "start kaputt");

    remove(MCONF); remove(DCONF);
}

void test_status_mapping()
{
    // Daemon aus: enabled entscheidet Disabled vs Disconnected.
    VpnStatus st = parse_status("", false, false);
    ICHECK(st.state == VpnState::Disabled && !st.daemon_running);
    st = parse_status("", false, true);
    ICHECK(st.state == VpnState::Disconnected);

    // Der Gutfall, Zeilen wie weirdikectl sie liefert.
    st = parse_status(
        "state=CHILD_SA_ESTABLISHED\ngateway=vpn.example.org:500\ninterface=ipsec0\n"
        "local_ts=10.77.0.2/32\nremote_ts=10.66.0.0/24\nnatt=yes\nnat_detected=yes\n"
        "child=aes256cbc-sha256\nchild_generation=2\nike_generation=1\nlast_notify=0\n"
        "uptime_s=42\ntx_packets=10\ntx_bytes=1200\nrx_packets=9\nrx_bytes=1100\n",
        true, true);
    ICHECK(st.state == VpnState::ChildEstablished);
    ICHECK(st.failure == VpnFailure::None);
    ICHECK(st.gateway == "vpn.example.org:500");
    ICHECK(st.interface_name == "ipsec0");
    ICHECK(st.nat_t && st.nat_detected);
    ICHECK(st.child_generation == 2 && st.ike_generation == 1);
    ICHECK(st.uptime_s == 42 && st.tx_bytes == 1200 && st.rx_packets == 9);

    // Zwischenzustaende.
    ICHECK(parse_status("state=SA_INIT_SENT\n", true, true).state == VpnState::Connecting);
    ICHECK(parse_status("state=AUTH_SENT\n", true, true).state == VpnState::Connecting);
    ICHECK(parse_status("state=IKE_SA_ESTABLISHED\n", true, true).state == VpnState::IkeEstablished);
    ICHECK(parse_status("state=CLOSED\n", true, true).state == VpnState::Disconnected);

    // Fehlerklassifikation: WeirdIKEs Diag-Trennung bleibt getrennt.
    st = parse_status("state=FAILED\nlast_notify=24\n", true, true);
    ICHECK(st.state == VpnState::Failed && st.failure == VpnFailure::AuthenticationFailed);
    st = parse_status("state=FAILED\nlast_notify=14\n", true, true);
    ICHECK(st.failure == VpnFailure::NoProposalChosen);
    st = parse_status("state=FAILED\nlast_notify=38\n", true, true);
    ICHECK(st.failure == VpnFailure::TsUnacceptable);
    st = parse_status("state=FAILED\nlast_notify=0\n", true, true);
    ICHECK(st.failure == VpnFailure::TransportTimeout);
    st = parse_status("state=FAILED\nlast_notify=17\n", true, true);
    ICHECK(st.failure == VpnFailure::Other);

    // Ein UNBEKANNTER Zustand faellt auf Failed, nie auf etwas Gruenes.
    ICHECK(parse_status("state=SOMETHING_NEW\n", true, true).state == VpnState::Failed);

    // Namen sind stabil (API-Vertrag).
    ICHECK(std::string(vpn_state_name(VpnState::ChildEstablished)) == "childEstablished");
    ICHECK(std::string(vpn_failure_name(VpnFailure::AuthenticationFailed)) == "authenticationFailed");
}

void test_review_findings()
{
    // Review-Fund 1: PSK-Regeln. Ein \n waere eine Config-Zeilen-Injection
    // in die Daemon-Datei, Randleerzeichen wuerden vom Daemon-Parser still
    // wegtrimmt, >128 lehnte erst der Daemon-START ab.
    ICHECK(psk_check("").empty());
    ICHECK(psk_check("ganz-normal-42").empty());
    ICHECK(psk_check("mit innen raum").empty());
    ICHECK(!psk_check("boese\ngateway = evil.host").empty());
    ICHECK(!psk_check("rand ").empty());
    ICHECK(!psk_check(" rand").empty());
    ICHECK(!psk_check(std::string(65, 'x')).empty());
    ICHECK(psk_check(std::string(64, 'x')).empty());
    // ... und die Meldung traegt NIE den Wert.
    ICHECK(psk_check("boese\nzeile").find("boese") == std::string::npos);

    // set_config setzt das durch, BEVOR etwas geschrieben wird.
    remove(MCONF); remove(DCONF);
    FakeBackend be;
    IpsecService svc(be, MCONF, DCONF);
    ICHECK(!svc.set_config(sample(), S("x\npsk-injection")).empty());
    ICHECK(slurp(DCONF).empty());

    // Review-Fund 3: stirbt der Daemon zwischen zwei Aufrufen (ctl liefert
    // running=true, aber leeren Text), wird daraus KEIN failed fantasiert.
    ICHECK(svc.set_config(sample(), S("s3cret-psk")).empty());
    be.running = true; be.status_text = "";
    ICHECK(svc.status().state == VpnState::Disconnected);

    remove(MCONF); remove(DCONF);
}

void test_ap5_underlay_binding()
{
    remove(MCONF); remove(DCONF);
    FakeBackend be;
    FakeUplinks up;
    IpsecService svc(be, MCONF, DCONF, &up);

    IpsecConfig c = sample();
    c.underlay = Underlay::Cellular;
    ICHECK(svc.set_config(c, S("s3cret-psk")).empty());

    // cellular unbrauchbar: Connect VERWEIGERT, nichts gestartet, keine Route.
    up.cell.usable = false;
    ICHECK(!svc.connect().empty());
    ICHECK(be.starts == 0 && be.routes.empty());

    // cellular da: Daemon-Datei traegt die KONKRETE Cellular-Bindung und das
    // VORAB aufgeloeste Gateway-Literal; die Peer-Route liegt auf usb0.
    up.cell.usable = true;
    ICHECK(svc.connect().empty());
    std::string d = slurp(DCONF);
    ICHECK(d.find("bind_ip = 100.71.3.9\n") != std::string::npos);
    ICHECK(d.find("bind_dev = usb0\n") != std::string::npos);
    ICHECK(d.find("gateway = 203.0.113.5\n") != std::string::npos);
    ICHECK(d.find("psk = s3cret-psk\n") != std::string::npos);   // write-only weitergetragen
    ICHECK(be.routes.size() == 1 && be.routes[0].ip == "203.0.113.5"
           && be.routes[0].dev == "usb0" && be.routes[0].gw == "100.71.3.1");

    VpnStatus st = svc.status();
    ICHECK(st.requested_underlay == "cellular");
    ICHECK(st.actual_underlay == "cellular");
    ICHECK(st.underlay_interface == "usb0" && st.underlay_ipv4 == "100.71.3.9");
    ICHECK(st.peer_ipv4 == "203.0.113.5");

    // Disconnect raeumt die Peer-Route restlos.
    ICHECK(svc.disconnect().empty());
    ICHECK(be.routes.empty());
    ICHECK(svc.status().actual_underlay.empty());

    // ethernet gewuenscht: Socket an A, nicht B.
    c.underlay = Underlay::Ethernet;
    ICHECK(svc.set_config(c, S("")).empty());
    ICHECK(svc.connect().empty());
    d = slurp(DCONF);
    ICHECK(d.find("bind_ip = 10.0.0.5\n") != std::string::npos);
    ICHECK(d.find("bind_dev = eth0\n") != std::string::npos);
    ICHECK(be.routes.size() == 1 && be.routes[0].dev == "eth0");
    ICHECK(svc.disconnect().empty() && be.routes.empty());

    // DNS-Fehlschlag: verweigert, BEVOR eine Route existiert.
    be.resolve_ok = false;
    ICHECK(!svc.connect().empty());
    ICHECK(be.routes.empty());
    be.resolve_ok = true;

    remove(MCONF); remove(DCONF);
}

void test_ap5_underlay_loss()
{
    remove(MCONF); remove(DCONF);
    FakeBackend be;
    FakeUplinks up;
    IpsecService svc(be, MCONF, DCONF, &up);

    IpsecConfig c = sample();
    c.underlay = Underlay::Cellular;
    ICHECK(svc.set_config(c, S("s3cret-psk")).empty());
    ICHECK(svc.connect().empty());
    be.status_text = "state=CHILD_SA_ESTABLISHED\n";

    // Solange das Underlay unveraendert ist, tut tick() nichts.
    svc.tick(1000);
    ICHECK(be.running && be.routes.size() == 1);

    // Cellular faellt weg: Abbau (Daemon stop, Route weg) und Failed/
    // underlayLost -- KEIN stiller Wechsel auf ethernet.
    up.cell.usable = false;
    svc.tick(2000);
    ICHECK(!be.running);
    ICHECK(be.routes.empty());
    be.status_text.clear();
    VpnStatus st = svc.status();
    ICHECK(st.state == VpnState::Failed);
    ICHECK(st.failure == VpnFailure::UnderlayLost);
    ICHECK(std::string(vpn_failure_name(st.failure)) == "underlayLost");

    // Neue Adresse nach Reconnect des Modems = neues Underlay: erst ein
    // NEUER connect nimmt es (kein MOBIKE-Vortaeuschen).
    up.cell.usable = true;
    up.cell.ipv4 = "100.71.9.1";
    ICHECK(svc.connect().empty());
    ICHECK(slurp(DCONF).find("bind_ip = 100.71.9.1\n") != std::string::npos);
    ICHECK(svc.status().state != VpnState::Failed);   // lost ist geloescht
    ICHECK(svc.disconnect().empty());

    remove(MCONF); remove(DCONF);
}

void test_ap6_routes_and_full_tunnel()
{
    // AP6: mehrere Routen aus dem Daemon-Status, source tsr|cp getrennt.
    VpnStatus st = parse_status(
        "state=CHILD_SA_ESTABLISHED\ninterface=ipsec0\n"
        "route=192.168.178.0/24 tsr ipsec0\n"
        "route=10.20.0.0/16 tsr ipsec0\n"
        "route=10.99.0.0/24 cp ipsec0\n",
        true, true);
    ICHECK(st.routes.size() == 3);
    ICHECK(st.routes[0].prefix == "192.168.178.0/24" && st.routes[0].source == "tsr");
    ICHECK(st.routes[0].device == "ipsec0");
    ICHECK(st.routes[2].source == "cp");
    ICHECK(!st.full_tunnel_refused);

    // full_tunnel_refused wird durchgereicht.
    st = parse_status("state=CHILD_SA_ESTABLISHED\nfull_tunnel_refused=yes\n", true, true);
    ICHECK(st.full_tunnel_refused);
    ICHECK(st.routes.empty());

    // AP6: die Config darf mehrere remote-Netze anfordern (komma-getrennt);
    // jedes muss ein gueltiges CIDR sein, hoechstens vier.
    IpsecConfig c = sample();
    c.remote_subnet = "192.168.178.0/24, 10.20.0.0/16";
    ICHECK(validate(c).empty());
    c.remote_subnet = "192.168.178.0/24, garbage";
    ICHECK(validate(c).find("garbage") != std::string::npos);
    c.remote_subnet = "1.0.0.0/8,2.0.0.0/8,3.0.0.0/8,4.0.0.0/8,5.0.0.0/8";
    ICHECK(!validate(c).empty());
    // ... und die Liste faehrt unveraendert in die Daemon-Datei (der Daemon
    // parst die Kommas).
    c.remote_subnet = "192.168.178.0/24,10.20.0.0/16";
    bool present = false;
    const std::string d = to_weirdike_conf(c, S("s3cret-psk"), "", &present);
    ICHECK(d.find("remote_subnet = 192.168.178.0/24,10.20.0.0/16\n") != std::string::npos);
}

void test_ap7_lifecycle()
{
    // Runtime-Zustand + Backoff-Delays (reine Funktion).
    ICHECK(reconnect_delay_ms(1) == 0);
    ICHECK(reconnect_delay_ms(2) == 2000);
    ICHECK(reconnect_delay_ms(3) == 5000);
    ICHECK(reconnect_delay_ms(4) == 10000);
    ICHECK(reconnect_delay_ms(9) == 30000);

    // Runtime-Ableitung: Child + Route = DataPlaneUp.
    remove(MCONF); remove(DCONF);
    FakeBackend be; FakeUplinks up;
    IpsecService svc(be, MCONF, DCONF, &up);
    IpsecConfig c = sample(); c.underlay = Underlay::Cellular;
    ICHECK(svc.set_config(c, S("s3cret-psk")).empty());
    ICHECK(svc.connect().empty());
    be.status_text = "state=CHILD_SA_ESTABLISHED\nchild_generation=1\n"
                     "route=10.66.0.0/24 tsr ipsec0\n";
    VpnStatus st = svc.status();
    ICHECK(st.runtime == VpnRuntimeState::DataPlaneUp);
    ICHECK(std::string(vpn_runtime_state_name(st.runtime)) == "dataPlaneUp");
    ICHECK(!st.manual_stop);

    // stabiler Tick setzt den Backoff auf 0.
    svc.tick(1000);
    ICHECK(be.running);

    // Underlay-Verlust -> Abbau + geplanter Reconnect (wiederherstellbar).
    up.cell.usable = false;
    svc.tick(2000);
    ICHECK(!be.running);
    st = svc.status();
    ICHECK(st.runtime == VpnRuntimeState::Failed);
    ICHECK(st.reconnect_attempt == 1);            // 1. Versuch geplant

    // Reconnect ist bei attempt 1 sofort faellig, aber cellular ist noch weg
    // -> connect() scheitert -> naechster Versuch geplant (attempt 2).
    svc.tick(2000);
    ICHECK(!be.running);
    ICHECK(svc.status().reconnect_attempt == 2);
    // attempt 2 = 2s Delay: ein Tick unmittelbar danach zuendet NICHT.
    svc.tick(2100);
    ICHECK(svc.status().reconnect_attempt == 2);

    // Underlay kommt zurueck: der faellige Reconnect verbindet.
    up.cell.usable = true;
    svc.tick(60000);
    ICHECK(be.running);
    be.status_text = "state=CHILD_SA_ESTABLISHED\nchild_generation=1\n"
                     "route=10.66.0.0/24 tsr ipsec0\n";
    svc.tick(61000);
    ICHECK(svc.status().reconnect_attempt == 0);   // stabil -> Reset

    // Manueller Stopp verbietet Auto-Reconnect.
    ICHECK(svc.disconnect().empty());
    ICHECK(svc.status().manual_stop);
    up.cell.usable = false;                         // egal
    svc.tick(62000);
    ICHECK(!be.running);
    ICHECK(svc.status().reconnect_attempt == 0);    // NICHTS geplant

    remove(MCONF); remove(DCONF);
}

void test_ap7_terminal_no_reconnect()
{
    // Ein Auth-Fehlschlag (Notify 24) ist terminal: kein Auto-Reconnect in
    // eine Wand. (Der Daemon meldet FAILED; tick baut ab, plant aber nichts,
    // weil ein FAILED mit Auth-Notify nicht durch Wiederholen heilt --
    // hier ueber die Config-Gate simuliert: kein PSK -> nicht wiederherstellbar.)
    remove(MCONF); remove(DCONF);
    FakeBackend be; FakeUplinks up;
    IpsecService svc(be, MCONF, DCONF, &up);
    IpsecConfig c = sample(); c.underlay = Underlay::Cellular;
    ICHECK(svc.set_config(c, S("s3cret-psk")).empty());
    ICHECK(svc.connect().empty());

    // Daemon meldet FAILED (z.B. DPD/Peer weg) -> Abbau + Reconnect geplant.
    be.status_text = "state=FAILED\nlast_notify=0\n";
    svc.tick(1000);
    ICHECK(!be.running);
    ICHECK(svc.status().reconnect_attempt == 1);

    // Jetzt wird die Config unbrauchbar (PSK-Datei weg): der naechste
    // faellige Reconnect-Versuch plant NICHTS mehr.
    remove(DCONF);                                  // psk_set() -> false
    svc.tick(2000);
    ICHECK(svc.status().reconnect_attempt == 0);

    remove(MCONF); remove(DCONF);
}

void test_ap9_eap_mschapv2()
{
    // Auth/Trust-Namen matchen die WeirdIKE-Typen.
    Auth a; TrustMode t;
    ICHECK(auth_from_name("eap-mschapv2", a) && a == Auth::EapMschapv2);
    ICHECK(!auth_from_name("kerberos", a));
    ICHECK(trust_mode_from_name("host-store", t) && t == TrustMode::HostStore);
    ICHECK(trust_mode_from_name("anchor-pem", t) && t == TrustMode::AnchorPem);
    ICHECK(!trust_mode_from_name("trust-me", t));

    // validate: EAP braucht eine Identity, wenn enabled.
    IpsecConfig c = sample();
    c.auth = Auth::EapMschapv2; c.eap_user.clear();
    ICHECK(validate(c).find("eapUser") != std::string::npos);
    c.eap_user = "user@example";
    ICHECK(validate(c).empty());

    // machino-Datei traegt auth/eap_user/trust_mode, aber NIE ein Passwort.
    c.trust_mode = TrustMode::AnchorPem;
    const std::string m = to_machino_conf(c);
    ICHECK(m.find("auth = eap-mschapv2") != std::string::npos);
    ICHECK(m.find("eap_user = user@example") != std::string::npos);
    ICHECK(m.find("trust_mode = anchor-pem") != std::string::npos);
    ICHECK(m.find("eap_password") == std::string::npos);

    // Daemon-Datei: eap_password write-only (neu -> carry), plus trust_mode
    // und die PEM-Pfade nur bei EAP.
    IpsecSecrets sec; sec.eap_password = "s3cret-pw";
    bool present = false;
    const std::string d1 = to_weirdike_conf(c, sec, "", &present, nullptr,
                                            "/etc/weirdike/ca.pem", "");
    ICHECK(present);
    ICHECK(d1.find("auth = eap-mschapv2") != std::string::npos);
    ICHECK(d1.find("eap_password = s3cret-pw\n") != std::string::npos);
    ICHECK(d1.find("trust_mode = anchor-pem") != std::string::npos);
    ICHECK(d1.find("ca_pem_file = /etc/weirdike/ca.pem") != std::string::npos);
    ICHECK(d1.find("psk =") == std::string::npos);      // kein PSK bei EAP

    // Zweiter Schreibvorgang ohne neues Passwort traegt das alte weiter.
    IpsecSecrets empty;
    const std::string d2 = to_weirdike_conf(c, empty, d1, &present, nullptr, "", "");
    ICHECK(present);
    ICHECK(d2.find("eap_password = s3cret-pw\n") != std::string::npos);

    // Service: EAP-Presence, PEM 0600-Dateien, connect braucht user+password.
    remove(MCONF); remove(DCONF); remove("ca.pem"); remove("extra.pem");
    FakeBackend be; FakeUplinks up;
    IpsecService svc(be, MCONF, DCONF, &up);
    IpsecConfig ec = sample(); ec.underlay = Underlay::Cellular;
    ec.auth = Auth::EapMschapv2; ec.eap_user = "user@example";
    ec.trust_mode = TrustMode::AnchorPem;

    // enabled ohne Passwort: abgelehnt, bevor etwas persistiert.
    ICHECK(!svc.set_config(ec, IpsecSecrets{}).empty());

    IpsecSecrets s2; s2.eap_password = "s3cret-pw"; s2.ca_pem = "-----BEGIN CERTIFICATE-----\nX\n-----END CERTIFICATE-----\n";
    ICHECK(svc.set_config(ec, s2).empty());
    ICHECK(svc.eap_password_set());
    ICHECK(svc.ca_pem_set());
    ICHECK(!svc.psk_set());
    // Secret nie in der machino-Datei.
    ICHECK(slurp(MCONF).find("s3cret-pw") == std::string::npos);
    ICHECK(slurp(DCONF).find("eap_password = s3cret-pw") != std::string::npos);

    // connect: EAP-Pfad. (Kein PSK noetig.)
    ICHECK(svc.connect().empty());
    ICHECK(be.running);

    // AP7xAP9-Regression: ein EAP-Tunnel MUSS auch reconnecten. Frueher
    // gatete schedule_reconnect_ nur auf psk_set() -> ein EAP-Tunnel (psk
    // leer) haette nach Underlay-Verlust NIE einen Reconnect geplant.
    be.status_text = "state=CHILD_SA_ESTABLISHED\n";
    up.cell.usable = false;
    svc.tick(1000);
    ICHECK(!be.running);
    ICHECK(svc.status().reconnect_attempt == 1);   // EAP plant den Reconnect
    up.cell.usable = true;
    ICHECK(svc.disconnect().empty());

    // Ein EAP-Config OHNE gesetztes Passwort (frische Dateien) -> connect
    // verweigert mit klarer Meldung, kein stiller PSK-Fallback.
    remove(MCONF); remove(DCONF);
    IpsecService svc2(be, MCONF, DCONF, &up);
    ICHECK(svc2.set_config(ec, s2).empty());
    remove(DCONF);                                   // Passwort weg
    ICHECK(svc2.connect() == "kein EAP-Passwort gesetzt");

    remove(MCONF); remove(DCONF);
    remove("ca.pem"); remove("extra.pem");   // ca_pem_path_ liegt neben DCONF (hier: cwd)
}

} // namespace

// AP10 (WeirdOS-Profilparitaet): ID-Typen, Tunnel-Adresse per CP, PFS,
// Autostart -- Persistenz, Validierung, die Daemon-Datei (byteidentisch fuer
// eine unveraenderte Konfiguration) und der Autostart am Fake-Backend.
void test_ap10_profile_parity()
{
    // request_cp ist abgeleitet: PSK ohne local_subnet, EAP immer.
    IpsecConfig c = sample();
    ICHECK(!request_cp(c));                    // sample hat local_subnet
    c.local_subnet.clear();
    ICHECK(request_cp(c));
    c = sample(); c.auth = Auth::EapMschapv2; c.eap_user = "u@x";
    ICHECK(request_cp(c));

    // Roundtrip der neuen Felder in der machino-Datei.
    c = sample();
    c.local_id = "NOTBETR_KA2@intern"; c.local_id_type = IdType::Rfc822;
    c.remote_id = "203.0.113.5";       c.remote_id_type = IdType::Ipv4;
    c.pfs = true; c.auto_connect = false;
    ICHECK(validate(c).empty());
    IpsecConfig back; std::string err;
    ICHECK(from_machino_conf(to_machino_conf(c), back, err));
    ICHECK(back.local_id_type == IdType::Rfc822 && back.remote_id_type == IdType::Ipv4);
    ICHECK(back.pfs && !back.auto_connect);
    // Eine ALTE machino-Datei ohne die Keys: Vorgaben = fqdn, kein PFS, Autostart an.
    ICHECK(from_machino_conf("enabled = false\ngateway = g\nport = 500\n", back, err));
    ICHECK(back.local_id_type == IdType::Fqdn && back.remote_id_type == IdType::Fqdn && !back.pfs && back.auto_connect);
    ICHECK(!from_machino_conf("local_id_type = email\n", back, err) && err.find("local_id_type") != std::string::npos);

    // Der Typ muss zum Wert passen.
    c = sample(); c.local_id = "cam.example"; c.local_id_type = IdType::Ipv4;
    ICHECK(validate(c).find("localId") != std::string::npos && validate(c).find("ipv4") != std::string::npos);
    c.local_id = "10.0.0.7";
    ICHECK(validate(c).empty());
    c = sample(); c.remote_id = "vpn.test"; c.remote_id_type = IdType::Ipv4;
    ICHECK(validate(c).find("remoteId") != std::string::npos);
    IdType t;
    ICHECK(id_type_from_name("keyid", t) && t == IdType::KeyId && !id_type_from_name("email", t));

    // Daemon-Datei: unveraenderte Konfiguration -> KEINE neuen Zeilen (ein
    // aelterer weirdiked liest sie weiterhin).
    bool present = false;
    std::string d = to_weirdike_conf(sample(), S("s3cret-psk"), "", &present);
    ICHECK(d.find("_id_type") == std::string::npos);
    ICHECK(d.find("request_cp") == std::string::npos);
    ICHECK(d.find("pfs_group") == std::string::npos);
    ICHECK(d.find("local_subnet = 10.77.0.2/32\n") != std::string::npos);

    // Das WeirdOS-Profil: RFC822-ID, leere Tunnel-Adresse (CP), PFS.
    c = sample();
    c.local_id = "NOTBETR_KA2@intern"; c.local_id_type = IdType::Rfc822;
    c.remote_id.clear();
    c.local_subnet.clear();
    c.pfs = true;
    d = to_weirdike_conf(c, S("s3cret-psk"), "", &present);
    ICHECK(d.find("local_id = NOTBETR_KA2@intern\n") != std::string::npos);
    ICHECK(d.find("local_id_type = rfc822\n") != std::string::npos);
    ICHECK(d.find("remote_id") == std::string::npos);          // leer = jede, keine Typzeile
    ICHECK(d.find("local_subnet") == std::string::npos);
    ICHECK(d.find("request_cp = yes\n") != std::string::npos);
    ICHECK(d.find("pfs_group = 14\n") != std::string::npos);
    ICHECK(d.find("s3cret-psk") != std::string::npos);
    // EAP fordert CP immer an, auch mit local_subnet.
    c = sample(); c.auth = Auth::EapMschapv2; c.eap_user = "u@x";
    IpsecSecrets sec; sec.eap_password = "pw";
    d = to_weirdike_conf(c, sec, "", &present);
    ICHECK(d.find("request_cp = yes\n") != std::string::npos && d.find("local_subnet = 10.77.0.2/32\n") != std::string::npos);

    // Status: die neuen Daemon-Zeilen.
    VpnStatus st = parse_status("state=CHILD_SA_ESTABLISHED\ntunnel_ip=10.9.0.7\nrequest_cp=yes\ncp_address=10.9.0.7\n"
                                "pfs_group=14\nlocal_id_type=rfc822\nremote_id_type=fqdn\n", true, true);
    ICHECK(st.tunnel_ipv4 == "10.9.0.7" && st.request_cp && st.cp_address == "10.9.0.7");
    ICHECK(st.pfs_group == 14 && st.local_id_type == "rfc822" && st.remote_id_type == "fqdn");
    st = parse_status("state=IDLE\n", true, true);
    ICHECK(st.tunnel_ipv4.empty() && !st.request_cp && st.pfs_group == 0);

    // Autostart: enabled + auto_connect -> der erste tick verbindet.
    remove(MCONF); remove(DCONF);
    {
        FakeBackend be;
        IpsecService svc(be, MCONF, DCONF);
        ICHECK(svc.set_config(sample(), S("s3cret-psk")).empty());
        svc.tick(1000);
        ICHECK(be.starts == 1 && be.running);
        ICHECK(svc.status().runtime != VpnRuntimeState::Idle);
        svc.tick(2000);
        ICHECK(be.starts == 1);                       // genau einmal
    }
    // auto_connect=false: nichts passiert.
    remove(MCONF); remove(DCONF);
    {
        FakeBackend be;
        IpsecService svc(be, MCONF, DCONF);
        IpsecConfig off = sample(); off.auto_connect = false;
        ICHECK(svc.set_config(off, S("s3cret-psk")).empty());
        svc.tick(1000); svc.tick(5000);
        ICHECK(be.starts == 0);
    }
    // enabled=false: nichts passiert, auch mit auto_connect.
    remove(MCONF); remove(DCONF);
    {
        FakeBackend be;
        IpsecService svc(be, MCONF, DCONF);
        IpsecConfig dis = sample(); dis.enabled = false;
        ICHECK(svc.set_config(dis, S("s3cret-psk")).empty());
        svc.tick(1000);
        ICHECK(be.starts == 0);
    }
    // Kein PSK: kein Versuch (schedule_reconnect_ verlangt das Credential).
    remove(MCONF); remove(DCONF);
    {
        FakeBackend be;
        IpsecService svc(be, MCONF, DCONF);
        IpsecConfig dis = sample(); dis.enabled = false;
        ICHECK(svc.set_config(dis, S("")).empty());
        svc.tick(1000);
        ICHECK(be.starts == 0);
    }
    // Daemon laeuft schon (machinod-Neustart): nicht anfassen.
    remove(MCONF); remove(DCONF);
    {
        FakeBackend be; be.running = true;
        IpsecService svc(be, MCONF, DCONF);
        ICHECK(svc.set_config(sample(), S("s3cret-psk")).empty());
        svc.tick(1000);
        ICHECK(be.starts == 0 && be.stops == 0);
    }
    // Der Start scheitert (Underlay noch nicht da): Backoff-Kette, kein Hammer.
    remove(MCONF); remove(DCONF);
    {
        FakeBackend be; be.start_ok = false;
        IpsecService svc(be, MCONF, DCONF);
        ICHECK(svc.set_config(sample(), S("s3cret-psk")).empty());
        svc.tick(1000);
        ICHECK(be.starts == 1);
        svc.tick(1500);
        ICHECK(be.starts == 1);                       // Versuch 2 erst nach 2 s (+Jitter)
        svc.tick(1000 + 2000 + 600);
        ICHECK(be.starts == 2);
        be.start_ok = true;
        svc.tick(1000 + 2000 + 600 + 5000 + 1300);    // Versuch 3 nach 5 s (+Jitter)
        ICHECK(be.starts == 3 && be.running);
    }
    remove(MCONF); remove(DCONF);
}

// AP11: die LANCOM-Matrix -- Katalog, Allow-Listen, Ablehnung MIT NAMEN,
// Config = Kabel (Daemon-Datei), PFS mit der kleinsten Gruppe, die
// Liveness-Knoepfe, die ausgehandelten Suiten im Status, Rekey.
void test_ap11_algorithm_grid()
{
    // Katalog: das komplette Raster, Vorgabe-Suite implementiert.
    size_t n = 0;
    const AlgoInfo* dh = algo_table(AlgoGroup::Dh, n);
    ICHECK(n == 13 && std::string(dh[0].id) == "dh2" && std::string(dh[12].id) == "dh32");
    ICHECK(algo_find(AlgoGroup::Dh, "dh14") && algo_find(AlgoGroup::Dh, "dh14")->implemented && algo_find(AlgoGroup::Dh, "dh14")->lancom_default);
    ICHECK(algo_find(AlgoGroup::Dh, "dh2") && !algo_find(AlgoGroup::Dh, "dh2")->implemented);
    ICHECK(algo_find(AlgoGroup::IkeEnc, "chacha20") && !algo_find(AlgoGroup::IkeEnc, "chacha20")->implemented);
    ICHECK(algo_find(AlgoGroup::IkeHash, "sha512") && algo_find(AlgoGroup::IkeHash, "sha512")->implemented);
    ICHECK(algo_find(AlgoGroup::EspEnc, "null") && !algo_find(AlgoGroup::EspEnc, "null")->implemented);
    ICHECK(!algo_find(AlgoGroup::EspHash, "blake2"));
    ICHECK(algo_smallest_dh({"dh19", "dh14", "dh31"}) == 14 && algo_smallest_dh({"dh31"}) == 31 && algo_smallest_dh({}) == 0);

    // Listen: alles Implementierte geht, der Rest wird MIT NAMEN abgelehnt.
    IpsecConfig c = sample();
    c.ike_dh = {"dh14", "dh19", "dh31"}; c.ike_enc = {"aes128cbc", "aes256cbc"}; c.ike_hash = {"sha1", "sha256", "sha512"};
    c.esp_enc = {"aes256cbc"}; c.esp_hash = {"sha1", "sha256"};
    ICHECK(validate(c).empty());
    c = sample(); c.ike_enc = {"chacha20"};
    std::string e = validate(c);
    ICHECK(e.find("ikeEnc") != std::string::npos && e.find("chacha20") != std::string::npos && e.find("nicht implementiert") != std::string::npos);
    c = sample(); c.ike_dh = {"dh14", "dh2"};
    ICHECK(validate(c).find("dh2") != std::string::npos);
    c = sample(); c.esp_hash = {"sha256", "sha3"};
    ICHECK(validate(c).find("unbekannt") != std::string::npos && validate(c).find("sha3") != std::string::npos);
    c = sample(); c.ike_hash.clear();
    ICHECK(validate(c).find("ikeHash") != std::string::npos && validate(c).find("leer") != std::string::npos);

    // machino-Datei: Listen und Knoepfe hin und zurueck; alte Datei mit
    // "ike ="/"esp =" liest sich weiter (Vorgaben).
    c = sample();
    c.ike_dh = {"dh14", "dh19"}; c.ike_hash = {"sha256", "sha1"};
    c.dpd = false; c.dpd_retries = 8; c.natt_keepalive_s = 15; c.child_lifetime_mb = 512; c.mtu = 1300;
    IpsecConfig back; std::string err;
    ICHECK(from_machino_conf(to_machino_conf(c), back, err));
    ICHECK(back.ike_dh == std::vector<std::string>({"dh14", "dh19"}) && back.ike_hash == std::vector<std::string>({"sha256", "sha1"}));
    ICHECK(!back.dpd && back.dpd_retries == 8 && back.natt_keepalive_s == 15 && back.child_lifetime_mb == 512 && back.mtu == 1300);
    ICHECK(from_machino_conf("enabled = false\ngateway = g\nike = aes256cbc,sha256,dh14\nesp = aes256cbc,sha256\n", back, err));
    ICHECK(back.ike_dh == std::vector<std::string>({"dh14"}) && back.dpd && back.mtu == 1400 && back.child_lifetime_mb == 0);
    c = sample(); c.mtu = 100;
    ICHECK(validate(c).find("mtu") != std::string::npos);
    c = sample(); c.natt_keepalive_s = 3;
    ICHECK(validate(c).find("nattKeepaliveS") != std::string::npos);

    // Daemon-Datei: Config = Kabel -- die Listen stehen IMMER drin, die
    // Knoepfe nur, wenn nicht Vorgabe; PFS nimmt die kleinste Gruppe.
    bool present = false;
    std::string d = to_weirdike_conf(sample(), S("s3cret-psk"), "", &present);
    ICHECK(d.find("ike_dh = dh14\n") != std::string::npos && d.find("ike_enc = aes256cbc\n") != std::string::npos);
    ICHECK(d.find("ike_hash = sha256\n") != std::string::npos && d.find("esp_enc = aes256cbc\n") != std::string::npos && d.find("esp_hash = sha256\n") != std::string::npos);
    ICHECK(d.find("dpd = no") == std::string::npos && d.find("dpd_retries") == std::string::npos);
    ICHECK(d.find("mtu") == std::string::npos && d.find("child_lifetime_kb") == std::string::npos && d.find("natt_keepalive") == std::string::npos);
    c = sample();
    c.ike_dh = {"dh19", "dh14"}; c.ike_hash = {"sha256", "sha1"}; c.pfs = true;
    c.dpd = false; c.dpd_retries = 8; c.natt_keepalive_s = 15; c.child_lifetime_mb = 2; c.mtu = 1300;
    d = to_weirdike_conf(c, S("s3cret-psk"), "", &present);
    ICHECK(d.find("ike_dh = dh19,dh14\n") != std::string::npos && d.find("ike_hash = sha256,sha1\n") != std::string::npos);
    ICHECK(d.find("pfs_group = 14\n") != std::string::npos);      // kleinste erlaubte Gruppe
    ICHECK(d.find("dpd = no\n") != std::string::npos && d.find("dpd_retries = 8\n") != std::string::npos);
    ICHECK(d.find("natt_keepalive_s = 15\n") != std::string::npos && d.find("child_lifetime_kb = 2048\n") != std::string::npos);
    ICHECK(d.find("mtu = 1300\n") != std::string::npos);
    c.ike_dh = {"dh31"};
    d = to_weirdike_conf(c, S("s3cret-psk"), "", &present);
    ICHECK(d.find("pfs_group = 31\n") != std::string::npos);

    // Status: die ausgehandelten Suiten.
    VpnStatus st = parse_status("state=CHILD_SA_ESTABLISHED\nike_suite=aes256cbc/sha256/sha256/dh14\nchild_suite=aes256cbc/sha256\n", true, true);
    ICHECK(st.ike_suite == "aes256cbc/sha256/sha256/dh14" && st.child_suite == "aes256cbc/sha256");

    // Rekey: nur mit laufendem Daemon; die Backend-Antwort zaehlt.
    remove(MCONF); remove(DCONF);
    {
        struct RekeyBackend : FakeBackend {
            int rekeys = 0, ike_rekeys = 0; bool accept = true;
            bool rekey(bool ike_sa, std::string& out) override {
                (ike_sa ? ike_rekeys : rekeys)++;
                out = accept ? "ok: requested\n" : "error: refused\n";
                return accept;
            }
        } be;
        IpsecService svc(be, MCONF, DCONF);
        ICHECK(svc.set_config(sample(), S("s3cret-psk")).empty());
        ICHECK(svc.rekey(false).find("kein Tunnel") != std::string::npos && be.rekeys == 0);
        ICHECK(svc.connect().empty());
        ICHECK(svc.rekey(false).empty() && be.rekeys == 1);
        ICHECK(svc.rekey(true).empty() && be.ike_rekeys == 1);
        be.accept = false;
        ICHECK(svc.rekey(false) == "error: refused");
    }
    remove(MCONF); remove(DCONF);
}

// AP12: der Test-Ping durch den Tunnel. Was hier zaehlt: Ablehnung MIT GRUND
// (kein Tunnel, Ziel ausserhalb der INSTALLIERTEN Tunnelrouten -- das Echo
// ginge sonst nie durch den Tunnel), die Bindung an das Tunnel-Interface,
// und die Ziel-Historie auf dem Geraet: neuestes zuerst, ohne Doppelte,
// gedeckelt, persistent, vergessbar -- wie das WeirdOS-Dropdown (NVS).
void test_ap12_test_ping()
{
    remove(MCONF); remove(DCONF); remove("ipsec-ping-targets");
    FakeBackend be;
    IpsecService svc(be, MCONF, DCONF);
    ICHECK(svc.set_config(sample(), S("psk-geheim")).empty());

    // rein: Praefix- und Literal-Test
    ICHECK(ipv4_in_prefix("10.66.0.7", "10.66.0.0/24") && !ipv4_in_prefix("10.66.1.7", "10.66.0.0/24"));
    ICHECK(ipv4_in_prefix("1.2.3.4", "0.0.0.0/0") && ipv4_in_prefix("10.66.0.7", "10.66.0.7/32") && !ipv4_in_prefix("10.66.0.8", "10.66.0.7/32"));
    ICHECK(!ipv4_in_prefix("10.66.0.7", "10.66.0.0") && !ipv4_in_prefix("x", "10.66.0.0/24") && !ipv4_in_prefix("10.66.0.7", "10.66.0.0/33"));
    ICHECK(ipv4_literal("192.168.110.11") && !ipv4_literal("fritz.box") && !ipv4_literal("256.1.1.1") && !ipv4_literal("1.2.3.4 ") && !ipv4_literal(""));

    PingResult r;
    // Eingaben: MIT NAMEN abgelehnt, nichts gemerkt.
    ICHECK(svc.ping("fritz.box", 1, 2000, r).rfind("target", 0) == 0);
    ICHECK(svc.ping("10.66.0.7", 0, 2000, r).rfind("count", 0) == 0);
    ICHECK(svc.ping("10.66.0.7", 6, 2000, r).rfind("count", 0) == 0);
    ICHECK(svc.ping("10.66.0.7", 1, 50, r).rfind("timeoutMs", 0) == 0);
    ICHECK(svc.ping("10.66.0.7", 5, 5000, r).rfind("count * timeoutMs", 0) == 0);
    // Kein Tunnel: gesagt -- nicht als "keine Antwort" getarnt.
    ICHECK(svc.ping("10.66.0.7", 1, 2000, r) == "kein Tunnel (Daemon laeuft nicht)");
    ICHECK(be.pings == 0 && svc.ping_targets().empty());
    be.running = true;
    be.status_text = "state=IKE_SA_ESTABLISHED\ninterface=ipsec0\n";
    ICHECK(svc.ping("10.66.0.7", 1, 2000, r) == "Tunnel steht nicht (ikeEstablished)");
    be.status_text = "state=CHILD_SA_ESTABLISHED\n";
    ICHECK(svc.ping("10.66.0.7", 1, 2000, r) == "Tunnel ohne Interface -- der Daemon meldet keins");
    be.status_text = "state=CHILD_SA_ESTABLISHED\ninterface=ipsec0\n";
    ICHECK(svc.ping("10.66.0.7", 1, 2000, r).rfind("keine Tunnelroute installiert", 0) == 0);
    be.status_text = "state=CHILD_SA_ESTABLISHED\ninterface=ipsec0\nroute=10.66.0.0/24 tsr ipsec0\nroute=192.168.178.0/24 cp ipsec0\n";
    ICHECK(svc.ping("10.77.0.1", 1, 2000, r) == "10.77.0.1 liegt in keiner installierten Tunnelroute (10.66.0.0/24, 192.168.178.0/24)");
    ICHECK(be.pings == 0 && svc.ping_targets().empty());

    // Der Ping: an das Tunnel-Interface gebunden, mit count/timeout -- und gemerkt.
    ICHECK(svc.ping("10.66.0.7", 3, 1500, r).empty());
    ICHECK(be.pings == 1 && be.last_ping.target == "10.66.0.7" && be.last_ping.ifname == "ipsec0" && be.last_ping.count == 3 && be.last_ping.timeout_ms == 1500);
    ICHECK(r.sent == 3 && r.received == 3 && r.error.empty() && r.via == "ipsec0" && r.rtt_avg_ms == 45.0);
    ICHECK(svc.ping_targets() == (std::vector<std::string>{"10.66.0.7"}));
    // "keine Antwort" ist ein MESSERGEBNIS, kein Ablehnungsgrund -- das Ziel bleibt gemerkt.
    be.ping_ok = false;
    ICHECK(svc.ping("192.168.178.1", 1, 2000, r).empty() && r.received == 0 && r.error == "keine Antwort (2000 ms je Echo)" && r.via == "ipsec0");
    be.ping_ok = true;
    ICHECK(svc.ping_targets() == (std::vector<std::string>{"192.168.178.1", "10.66.0.7"}));
    // Wiederholung rueckt nach vorn, keine Doppelten.
    ICHECK(svc.ping("10.66.0.7", 1, 2000, r).empty());
    ICHECK(svc.ping_targets() == (std::vector<std::string>{"10.66.0.7", "192.168.178.1"}));
    // Gedeckelt (kPingHistoryMax), neuestes zuerst.
    for (int i = 10; i < 20; ++i) ICHECK(svc.ping("10.66.0." + std::to_string(i), 1, 2000, r).empty());
    const std::vector<std::string> t = svc.ping_targets();
    ICHECK(t.size() == kPingHistoryMax && t.front() == "10.66.0.19" && t.back() == "10.66.0.12");
    // Persistent auf dem Geraet: eine zweite Instanz liest dieselbe Liste (Datei neben ipsec.conf).
    { IpsecService again(be, MCONF, DCONF); ICHECK(again.ping_targets() == t); }
    ICHECK(slurp("ipsec-ping-targets").rfind("10.66.0.19\n10.66.0.18\n", 0) == 0);
    // Vergessen.
    ICHECK(svc.forget_ping_target("10.66.0.19").empty() && svc.ping_targets().front() == "10.66.0.18" && svc.ping_targets().size() == kPingHistoryMax - 1);
    ICHECK(svc.forget_ping_target("nope").rfind("target", 0) == 0);
    ICHECK(svc.forget_ping_target("10.9.9.9").empty());   // nicht drin: kein Fehler
    // Eine kaputte Zeile faellt still raus, der Rest bleibt, Doppelte werden gefaltet.
    { FILE* f = fopen("ipsec-ping-targets", "w"); if (f) { fputs("10.66.0.1\nkaputt\n\n10.66.0.2\n10.66.0.1\n", f); fclose(f); } }
    ICHECK(svc.ping_targets() == (std::vector<std::string>{"10.66.0.1", "10.66.0.2"}));

    remove(MCONF); remove(DCONF); remove("ipsec-ping-targets");
}

void run_ipsec_tests()
{
    test_review_findings();
    test_ap9_eap_mschapv2();
    test_ap5_underlay_binding();
    test_ap5_underlay_loss();
    test_ap6_routes_and_full_tunnel();
    test_ap7_lifecycle();
    test_ap7_terminal_no_reconnect();
    test_config_roundtrip();
    test_validate_names_the_problem();
    test_psk_write_only_carry();
    test_service_persists_and_guards();
    test_connect_disconnect();
    test_status_mapping();
    test_ap10_profile_parity();
    test_ap11_algorithm_grid();
    test_ap12_test_ping();
}
